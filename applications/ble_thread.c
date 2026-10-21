#include "ble_thread.h"
#include "nus.h"
#include "storage.h"
#include "storage_thread.h"
#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/printk.h>

#define BLE_MAX_RETRIES          5
#define BLE_PACKET_SIZE          244
#define BLE_DATA_SIZE            242
#define READ_STORAGE_MAX_RETRIES 10 /* Total read attempts, including the first. */

/* Fixed frame: TYPE(1), SEQ(2), LEN(1), DATA(238), CRC(2). */
#define BLE_HEADER_SIZE         4U
#define BLE_PAYLOAD_SIZE        (BLE_DATA_SIZE - BLE_HEADER_SIZE)
#define BLE_TYPE_DATA           0x02U
#define BLE_TYPE_END            0x05U
#define BLE_ACK_TIMEOUT_MS      2000 /* NOTE REVIEW: Tune this timeout for the phone app. */
#define READ_STORAGE_CMD        0xAAU
#define PIPELINE_QUIESCE_TIMEOUT_MS 5000U

/* Handle data written by the peer to the NUS RX characteristic. */
static void rx_received(struct bt_conn *conn, const uint8_t *data, uint16_t len);

static void rx_disconnected(struct bt_conn *conn, uint8_t reason);

// callback ble
static struct bt_nus_cb nus_callbacks = {
    .connected = NULL, 
    .disconnected = rx_disconnected,
    .received = rx_received,
    .sent = NULL, /* App ACK confirms reception; TX completion does not. */
    .send_enabled = NULL, /* bt_nus_send() checks the peer's subscription. */
};
enum received_code {
    NONE = 0,
    READ_STORAGE = 1,
    RECEIVED_ACK = 2,
    RECEIVED_NACK = 3,
};

enum reply_type {
    REPLY_ACK  = 0xA0,
    REPLY_NACK = 0xA1,
};

enum ble_app_state {
    ble_app_idle = 0,
    ble_app_received_cmd,
    ble_app_handle_cmd,
    ble_app_wait_reply,
    ble_app_done,
};

static struct {
    uint8_t code;
    bool received_cmd; // Flag to indicate if a command has been received
    bool verified_cmd; /* Storage extent validated for this session. */
    bool storage_locked; /* This BLE thread owns storage_mutex until cleanup. */
    bool success_send; // Flag to indicate if the reply has been sent successfully
    uint8_t current_state; // Current state of the BLE application
    bool ack_received; // Flag to indicate if an ACK has been received
    bool nack_received; // Flag to indicate if a NACK has been received
    bool ack_timeout; // Flag to indicate if an ACK timeout has occurred
    uint8_t retry_count;
    bool done;
    bool failed; /* End this session through DONE; the worker thread keeps running. */
} ble_app_flags;

/* Full 2048-byte page: 2046-byte payload and two stored CRC bytes. */
static uint8_t read_page[STORAGE_BLOCK_SIZE];

static struct {
    struct bt_conn *conn; /* Reference held for the current session. */
    uint32_t storage_end; /* Committed physical extent, including page CRCs. */
    uint32_t page_offset; /* Physical offset of the page currently being sent. */
    uint16_t page_pos;    /* 2046 means that the next page must be loaded. */
    uint16_t seq;         /* Preserve between sessions on the same connection. */
    uint16_t payload_len;
    int64_t deadline;     /* ACK deadline in uptime milliseconds. */
    uint8_t packet[BLE_PACKET_SIZE]; /* Retained unchanged until ACK or retry. */
} transfer;

// stuct for a event pushed in queue
struct ble_rx_event {
    struct bt_conn *conn;
    uint16_t seq;
    uint8_t code; /* NONE is a disconnect event; other values use received_code. */
};

/* Four bounded events; each queued connection holds one reference. */
K_MSGQ_DEFINE(ble_rx_queue, sizeof(struct ble_rx_event), 4, _Alignof(struct ble_rx_event));


static int encode_ble_packet(uint8_t *buffer);
/* Return: 0 = valid payload, 1 = erased page, negative = error. */
static int decode_storage_data(uint32_t offset, uint8_t *buffer);
static int decode_command(const uint8_t *data, uint16_t len, uint16_t *sequence);
static void queue_rx_event(struct bt_conn *conn, uint8_t code, uint16_t seq);
static void fsm_change_state(void);
static void fsm_handle_state(struct k_mutex *storage_mutex);
static void idle_handler(void);
static void received_cmd_handler(struct k_mutex *storage_mutex);
static void handle_cmd_handler(void);
static void wait_reply_handler(void);
static void done_handler(struct k_mutex *storage_mutex);


void ble_thread_init(void)
{
    int err;
    err = bt_init(&nus_callbacks);
    if (err) {
        printk("[BLE_THREAD] BLE init failed: %d\n", err);
        return;
    }
    printk("[BLE_THREAD] BLE initialized\n");
}

void ble_thread(void *p1, void *p2, void *p3)
{
    /* Use the shared mutex passed by the thread creator. */
    struct k_mutex *storage_mutex = (struct k_mutex *)p1;
    (void)p2;
    (void)p3;

    while (1) {
        /* Receive one queued BLE event for processing in this thread. */
        struct ble_rx_event event;
        /* Process active states immediately without waiting for an event. */
        k_timeout_t timeout = K_NO_WAIT;
        if (ble_app_flags.current_state == ble_app_idle) {
            /* Wait for a new event without polling while idle. */
            timeout = K_FOREVER;
        }
        else if (ble_app_flags.current_state == ble_app_wait_reply) {
            /* Calculate the remaining ACK wait time to preserve the original deadline. */
            int64_t remaining = transfer.deadline - k_uptime_get();
            /* Wait until an event arrives or the deadline expires; do not wait if expired. */
            timeout = remaining > 0 ? K_MSEC(remaining) : K_NO_WAIT;
        }

        if (k_msgq_get(&ble_rx_queue, &event, timeout) == 0) {
            if (event.code == READ_STORAGE) {
                if (ble_app_flags.current_state == ble_app_idle) {
                    transfer.conn = event.conn;
                    event.conn = NULL; /* Transfer the queue's reference to the session. */
                    ble_app_flags.code = READ_STORAGE;
                    ble_app_flags.received_cmd = true;
                } 
                else {
                    printk("[BLE_THREAD] Command ignored: busy\n");
                }
            } 
            /* Ignore replies from other connections to protect the active session. */
            else if (event.conn == transfer.conn) {
                if (event.code == NONE) {
                    ble_app_flags.failed = true;
                    printk("[BLE_THREAD] Session disconnected\n");
                } 
                else if (ble_app_flags.current_state == ble_app_wait_reply && event.seq == transfer.seq) {
                    /* Keep the first valid reply for this packet. */
                    if (!ble_app_flags.ack_received && !ble_app_flags.nack_received) {
                        ble_app_flags.ack_received = event.code == RECEIVED_ACK;
                        ble_app_flags.nack_received = event.code == RECEIVED_NACK;
                    }
                }
            }

            /* Release the event reference unless ownership moved to the session. */
            if (event.conn != NULL) {
                bt_conn_unref(event.conn);
            }
        }

        /* Skip normal work after failure, but allow session cleanup to run. */
        if (!ble_app_flags.failed || ble_app_flags.current_state == ble_app_done) {
            fsm_handle_state(storage_mutex);
        }
        /* Use the handler results to select the next state. */
        fsm_change_state();
    }
}

static int encode_ble_packet(uint8_t *buffer)
{
    if (buffer == NULL) {
        printk("[BLE_THREAD] Encode failed: NULL buffer\n");
        return -EINVAL;
    }
    /* CRC-16/CCITT-FALSE over the first 242 bytes. */
    uint16_t crc = crc16_itu_t(
        0xFFFFU,
        buffer,
        BLE_DATA_SIZE
    );
    /* Append CRC in little-endian order. */
    sys_put_le16(crc, buffer + BLE_DATA_SIZE);

    return 0;
}
/*
 * offset is page-aligned and relative to the storage data region.
 * buffer must hold at least STORAGE_BLOCK_DATA_SIZE bytes.
 * Call with decode_storage_data(page_offset, payload_buffer).
 * Use buffer only on return 0. On return 1, skip to the next page
 * within the read extent; an erased page does not end the transfer.
 */
static int decode_storage_data(uint32_t offset, uint8_t *buffer)
{
    int err = -EIO; /* Preserve the last failure after retries are exhausted. */

    if (buffer == NULL || offset % STORAGE_BLOCK_SIZE != 0U) {
        printk("[BLE_THREAD] Decode failed: invalid request, offset=%u\n",
               (unsigned int)offset);
        return -EINVAL;
    }

    for (uint8_t i = 0; i < READ_STORAGE_MAX_RETRIES; i++) {
        /* Read the payload and CRC together into the reusable page buffer. */
        err = storage_read(offset, read_page, sizeof(read_page));

        if (err == 0) {
            bool erased = true; /* Cleared by any programmed byte. */

            /* Include the stored CRC when checking for an erased page. */
            for (size_t j = 0; j < sizeof(read_page); j++) {
                if (read_page[j] != 0xFFU) {
                    erased = false;
                    break;
                }
            }

            if (erased) {
                /* Leave the output buffer unchanged and do not retry. */
                return 1;
            }

            /* Match the seed and reflected CRC used by storage_write(). */
            uint16_t calculated_crc = crc16_ccitt(
                0xFFFFU,
                read_page,
                STORAGE_BLOCK_DATA_SIZE
            );

            /* The final two bytes contain the little-endian stored CRC. */
            uint16_t stored_crc = sys_get_le16(read_page + STORAGE_BLOCK_DATA_SIZE);
            if (calculated_crc == stored_crc) {
                /* Copy only CRC-validated payload. */
                if (buffer != read_page) {
                    memcpy(buffer, read_page, STORAGE_BLOCK_DATA_SIZE);
                }
                return 0;
            }
            err = -EBADMSG;
            printk("[BLE_THREAD] CRC mismatch: offset=%u, attempt=%u\n",
                   (unsigned int)offset, (unsigned int)i + 1U);
        } 
        else {
            printk("[BLE_THREAD] Read failed: offset=%u, attempt=%u, err=%d\n",
                   (unsigned int)offset, (unsigned int)i + 1U, err);

            if (err == -EINVAL || err == -ERANGE || err == -EWOULDBLOCK) {
                /* Retrying cannot fix invalid arguments, range or context. */
                return err;
            }
        }
        /* Wait only when another attempt remains. */
        if (i + 1U < READ_STORAGE_MAX_RETRIES) {
            k_msleep(10);
        }
    }

    return err;
}

/* Frame: TYPE(1) + SEQ(2) + CRC(2), little-endian. */
static int decode_command(const uint8_t *data,
                          uint16_t len,
                          uint16_t *sequence){
    if (data == NULL || sequence == NULL || len != 5U) {
        return -EINVAL;
    }
    uint8_t type = data[0];
    uint16_t seq = sys_get_le16(data + 1U);
    uint16_t received_crc = sys_get_le16(data + 3U);
    uint16_t calculated_crc = crc16_itu_t(0xFFFFU, data, 3U);
    if (calculated_crc != received_crc) {
        return -EBADMSG;
    }
    int code;
    switch (type) {
    case READ_STORAGE_CMD:
        code = READ_STORAGE;
        break;

    case REPLY_ACK:
        code = RECEIVED_ACK;
        break;

    case REPLY_NACK:
        code = RECEIVED_NACK;
        break;

    default:
        return -EINVAL;
    }

    /* Return the sequence only for a valid packet. */
    *sequence = seq;
    return code;
}

/* Shared enqueue helper; callback-owned pointers never enter the FSM. */
static void queue_rx_event(struct bt_conn *conn, uint8_t code, uint16_t seq){
    struct ble_rx_event event = {
        .conn = bt_conn_ref(conn),
        .seq = seq,
        .code = code,
    };
    /* Enqueue without waiting; a nonzero return means enqueue failed. */
    if (k_msgq_put(&ble_rx_queue, &event, K_NO_WAIT) != 0) {
        /* Release the connection reference because the event was not queued. */
        bt_conn_unref(event.conn);

        /* Report that the queue could not accept this event. */
        printk("[BLE_THREAD] RX queue full\n");
    }
}

// callback rx of ble
static void rx_received(struct bt_conn *conn, const uint8_t *data, uint16_t len)
{
    uint16_t sequence;
    int code = decode_command(data, len, &sequence);
    if (code < 0) {
        printk("[BLE_THREAD] Invalid RX packet: %d\n", code);
        return;
    }
    // push event in queue
    queue_rx_event(conn, (uint8_t)code, sequence);
}

/* Abort the matching session without modifying thread-owned flags here. */
static void rx_disconnected(struct bt_conn *conn, uint8_t reason)
{
    (void)reason;
    queue_rx_event(conn, NONE, 0U);
}

static void fsm_change_state(void)
{
    if (ble_app_flags.failed) {
        ble_app_flags.current_state = ble_app_done;
        return;
    }

    switch (ble_app_flags.current_state) {
        case ble_app_idle:
            if (ble_app_flags.received_cmd) {
                ble_app_flags.current_state = ble_app_received_cmd;
            }
            break;
        case ble_app_received_cmd:
            /* Wait until command validation finishes. */
            if (ble_app_flags.verified_cmd) {
                ble_app_flags.current_state = ble_app_handle_cmd;
            } else {
                ble_app_flags.current_state = ble_app_done;
            }
            break;

        case ble_app_handle_cmd:
            /* Stay here while preparing data or waiting for storage. */
            if (ble_app_flags.success_send) {
                ble_app_flags.current_state = ble_app_wait_reply;
            }
            break;

        case ble_app_wait_reply:
            /* ACK/NACK flags require a valid CRC and matching sequence. */
            if (ble_app_flags.ack_received) {
                if (!ble_app_flags.done) {
                    ble_app_flags.current_state = ble_app_handle_cmd;
                } else {
                    ble_app_flags.current_state = ble_app_done;
                }
            } else if (ble_app_flags.nack_received || ble_app_flags.ack_timeout) {
                if (ble_app_flags.retry_count < BLE_MAX_RETRIES) {
                    ble_app_flags.current_state = ble_app_handle_cmd;
                } else {
                    ble_app_flags.current_state = ble_app_done;
                }
            }
            break;
        case ble_app_done:
            /* Failed unlock must not allow a new session. */
            if (!ble_app_flags.storage_locked) {
                ble_app_flags.current_state = ble_app_idle;
            }
            break;
        default:
            printk("[BLE_THREAD] Invalid FSM state: %u\n",
                   (unsigned int)ble_app_flags.current_state);
            ble_app_flags.current_state = ble_app_idle;
            break;
        }
}
static void fsm_handle_state(struct k_mutex *storage_mutex)
{
    switch (ble_app_flags.current_state) {
    case ble_app_idle:
        idle_handler();
        break;

    case ble_app_received_cmd:
        received_cmd_handler(storage_mutex);
        break;

    case ble_app_handle_cmd:
        handle_cmd_handler();
        break;

    case ble_app_wait_reply:
        wait_reply_handler();
        break;

    case ble_app_done:
        done_handler(storage_mutex);
        break;

    default:
        printk("[BLE_THREAD] No handler for state: %u\n",
               (unsigned int)ble_app_flags.current_state);
        break;
    }
}

static void idle_handler(void){
}

static void received_cmd_handler(struct k_mutex *storage_mutex)
{
    /* Keep validation false until the storage extent is checked. */
    ble_app_flags.verified_cmd = false;

    if (ble_app_flags.code != READ_STORAGE) {
        printk("[BLE_THREAD] Invalid command: %u\n",
               (unsigned int)ble_app_flags.code);
        return;
    }

    if (storage_mutex == NULL) {
        printk("[BLE_THREAD] No storage mutex\n");
        return;
    }

    /* Acquisition is currently idle. Drain the writer before taking Flash;
     * holding this mutex while flushing would block the writer itself.
     */
    int err = storage_thread_flush(PIPELINE_QUIESCE_TIMEOUT_MS);
    if (err != 0) {
        printk("[BLE_THREAD] Storage flush failed: %d\n", err);
        return;
    }
    err = k_mutex_lock(storage_mutex, K_MSEC(PIPELINE_QUIESCE_TIMEOUT_MS));
    if (err != 0) {
        printk("[BLE_THREAD] Storage lock failed: %d\n", err);
        return;
    }
    ble_app_flags.storage_locked = true;

    err = storage_get_data_size(&transfer.storage_end);
    if (err != 0) {
        printk("[BLE_THREAD] Storage extent failed: %d\n", err);
        goto unlock_storage;
    }
    if (transfer.storage_end % STORAGE_BLOCK_SIZE != 0U) {
        printk("[BLE_THREAD] Unaligned storage extent: %u\n",
               (unsigned int)transfer.storage_end);
        goto unlock_storage;
    }

    /* Start at the first page; the end position forces an initial page load. */
    transfer.page_offset = 0U;
    transfer.page_pos = STORAGE_BLOCK_DATA_SIZE;
    /* Retain ownership across reads, BLE retries and the erase after END ACK. */
    ble_app_flags.verified_cmd = true;
    return;

unlock_storage:
    /* Invalid sessions must release the lock before leaving validation. */
    err = k_mutex_unlock(storage_mutex);
    if (err != 0) {
        printk("[BLE_THREAD] Storage unlock failed: %d\n", err);
        ble_app_flags.failed = true;
    } else {
        ble_app_flags.storage_locked = false;
    }
}

static void handle_cmd_handler(void)
{
    if (transfer.conn == NULL) {
        printk("[BLE_THREAD] No session connection\n");
        ble_app_flags.failed = true;
        return;
    }
    /* The ATT notification header needs three bytes in addition to the frame. */
    uint16_t mtu = bt_gatt_get_mtu(transfer.conn);
    if (mtu < BLE_PACKET_SIZE + 3U) {
        printk("[BLE_THREAD] MTU too small: %u\n", (unsigned int)mtu);
        ble_app_flags.failed = true;
        return;
    }

    if (ble_app_flags.success_send) {
        /* Bound retransmissions while preserving the original frame and sequence. */
        if (ble_app_flags.retry_count >= BLE_MAX_RETRIES) {
            printk("[BLE_THREAD] Send retry limit: seq=%u\n",
                   (unsigned int)transfer.seq);
            ble_app_flags.failed = true;
            return;
        }
        ble_app_flags.retry_count++;
    } 
    else {
        /* Load a page only after all payload bytes in the previous page are ACKed. */
        if (transfer.page_pos == STORAGE_BLOCK_DATA_SIZE) {
            /* Send END once the committed storage extent has been consumed. */
            if (transfer.page_offset == transfer.storage_end) {
                ble_app_flags.done = true;
            } 
            else {
                /* Decode in place to avoid another 2046-byte payload buffer. */
                int err = decode_storage_data(transfer.page_offset, read_page);
                /* Skip erased pages because they contain no validated payload. */
                if (err == 1) {
                    transfer.page_offset += STORAGE_BLOCK_SIZE;
                    return; /* Skip the erased page, then process the next one. */
                }
                if (err != 0) {
                    printk("[BLE_THREAD] Page decode failed: offset=%u, err=%d\n",
                           (unsigned int)transfer.page_offset, err);
                    ble_app_flags.failed = true;
                    return;
                }
                transfer.page_pos = 0U;
            }
        }
        /* Clear the entire packet to avoid sending stale bytes in unused payload space. */
        memset(transfer.packet, 0, sizeof(transfer.packet));
        /* END carries no payload; DATA replaces this with its actual length. */
        transfer.payload_len = 0U;
        if (!ble_app_flags.done) {
            /* Limit the payload to the bytes remaining in the current page. */
            transfer.payload_len = MIN(BLE_PAYLOAD_SIZE, STORAGE_BLOCK_DATA_SIZE - transfer.page_pos);
            /* Copy page data into the BLE packet after its header. */
            memcpy(transfer.packet + BLE_HEADER_SIZE, read_page + transfer.page_pos, transfer.payload_len);
        }
        /* Set the frame type: END when finished, otherwise DATA. */
        transfer.packet[0] = ble_app_flags.done ? BLE_TYPE_END : BLE_TYPE_DATA;
        /* Store the sequence number in bytes 1-2, least significant byte first. */
        sys_put_le16(transfer.seq, transfer.packet + 1U);
        /* Store the number of valid payload bytes in byte 3. */
        transfer.packet[3] = transfer.payload_len;
        /* Append CRC so the receiver can detect a corrupted frame. */
        int encode_err = encode_ble_packet(transfer.packet);
        if (encode_err != 0) {
            printk("[BLE_THREAD] Frame encode failed: %d\n", encode_err);
            ble_app_flags.failed = true;
            return;
        }
    }
    /* Clear old replies before waiting for confirmation of this send attempt. */
    ble_app_flags.ack_received = false;
    ble_app_flags.nack_received = false;
    ble_app_flags.ack_timeout = false;
    /* Send the complete frame through BLE NUS; abort if the stack rejects it. */
    int err = bt_nus_send(transfer.conn, transfer.packet, sizeof(transfer.packet));
    if (err != 0) {
        printk("[BLE_THREAD] Send failed: %d\n", err);
        ble_app_flags.failed = true;
        return;
    }
    /* Set the ACK deadline to detect a missing reply and trigger a retry. */
    transfer.deadline = k_uptime_get() + BLE_ACK_TIMEOUT_MS;
    /* Mark the frame as sent and retain it for retransmission until ACK arrives. */
    ble_app_flags.success_send = true;
}

/* Advance only on matching ACK; retain the packet for NACK/timeout. */
static void wait_reply_handler(void)
{
    if (ble_app_flags.ack_received) {
        /* Advance only confirmed DATA; END does not consume storage bytes. */
        if (!ble_app_flags.done) {
            transfer.page_pos += transfer.payload_len;
            if (transfer.page_pos == STORAGE_BLOCK_DATA_SIZE) {
                transfer.page_offset += STORAGE_BLOCK_SIZE;
            }
        }
        else {
            /* Erase only after END is acknowledged to preserve data on transfer failure. */
            int err = storage_erase_all_datas();
            if (err != 0) {
                printk("[BLE_THREAD] Storage erase failed: %d\n", err);
                ble_app_flags.failed = true;
                return;
            }
        }

        transfer.seq++;
        ble_app_flags.success_send = false; /* Next handler prepares a new packet. */
        ble_app_flags.retry_count = 0U;
        return; /* Leave ACK set for fsm_change_state(). */
    }

    if (ble_app_flags.nack_received || k_uptime_get() >= transfer.deadline) {
        printk("[BLE_THREAD] %s: seq=%u, retries=%u\n",
               ble_app_flags.nack_received ? "NACK" : "ACK timeout",
               (unsigned int)transfer.seq,
               (unsigned int)ble_app_flags.retry_count);
        ble_app_flags.ack_timeout = !ble_app_flags.nack_received;
        if (ble_app_flags.retry_count >= BLE_MAX_RETRIES) {
            printk("[BLE_THREAD] Reply retry limit reached\n");
            ble_app_flags.failed = true;
            return;
        }
        /* Keep success_send, packet, sequence and page cursor for the retry. */
    }
}

static void done_handler(struct k_mutex *storage_mutex)
{
    /* Release Flash on session completion and abort alike. */
    if (ble_app_flags.storage_locked) {
        int err = k_mutex_unlock(storage_mutex);

        if (err != 0) {
            printk("[BLE_THREAD] Storage unlock failed: %d\n", err);
            ble_app_flags.failed = true;
            k_sleep(K_MSEC(100));
            return;
        }
        ble_app_flags.storage_locked = false;
    }

    /* Release the session reference without requesting a BLE disconnect. */
    if (transfer.conn != NULL) {
        bt_conn_unref(transfer.conn);
        transfer.conn = NULL;
    }

    /* Avoid immediately reusing the last sequence after an aborted session. */
    transfer.seq++;
    transfer.storage_end = 0U;
    transfer.page_offset = 0U;
    transfer.page_pos = STORAGE_BLOCK_DATA_SIZE;
    transfer.payload_len = 0U;
    transfer.deadline = 0;

    /* Clear session flags so the next command starts with no stale replies. */
    ble_app_flags.code = NONE;
    ble_app_flags.received_cmd = false;
    ble_app_flags.verified_cmd = false;
    ble_app_flags.success_send = false;
    ble_app_flags.ack_received = false;
    ble_app_flags.nack_received = false;
    ble_app_flags.ack_timeout = false;
    ble_app_flags.retry_count = 0U;
    ble_app_flags.done = false;
    ble_app_flags.failed = false;
}

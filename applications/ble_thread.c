#include "ble_thread.h"
#include "nus.h"
#include "storage.h"
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
    bool verified_cmd; // Flag to indicate if the received command has been verified
    bool success_send; // Flag to indicate if the reply has been sent successfully
    uint8_t current_state; // Current state of the BLE application
    bool ack_received; // Flag to indicate if an ACK has been received
    bool nack_received; // Flag to indicate if a NACK has been received
    bool ack_timeout; // Flag to indicate if an ACK timeout has occurred
    uint8_t retry_count;
    bool done;
    bool failed; /* Terminal session error, consumed by the FSM, and disconnected BLE*/
} ble_app_flags;

/* Full 2048-byte page: 2046-byte payload and two stored CRC bytes. */
static uint8_t read_page[STORAGE_BLOCK_SIZE];

/* NOTE REVIEW: Only the BLE thread owns this state; no mutex while writing is stopped. */
static struct {
    struct bt_conn *conn; /* Reference held for the current session. */
    uint32_t storage_end; /* Committed physical extent, including page CRCs. */
    uint32_t page_offset; /* Physical offset of the page currently being sent. */
    uint16_t page_pos;    /* 2046 means that the next page must be loaded. */
    uint16_t seq;         /* Preserve between sessions on the same connection. */
    uint16_t payload_len
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
static void fsm_handle_state(void);
static void idle_handler(void);
static void received_cmd_handler(void);
static void handle_cmd_handler(void);
static void wait_reply_handler(void);
static void done_handler(void);


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
    (void)p1;
    (void)p2;
    (void)p3;

    /* NOTE REVIEW: Run immediate states without polling; sleep for RX or an ACK deadline. */
    for (;;) {
        struct ble_rx_event event;
        k_timeout_t timeout = K_NO_WAIT;

        if (ble_app_flags.current_state == ble_app_idle) {
            timeout = K_FOREVER;
        } else if (ble_app_flags.current_state == ble_app_wait_reply) {
            int64_t remaining = transfer.deadline - k_uptime_get();
            timeout = remaining > 0 ? K_MSEC(remaining) : K_NO_WAIT;
        }

        if (k_msgq_get(&ble_rx_queue, &event, timeout) == 0) {
            if (event.code == READ_STORAGE) {
                if (ble_app_flags.current_state == ble_app_idle) {
                    transfer.conn = event.conn;
                    event.conn = NULL; /* Transfer the queue's reference to the session. */
                    ble_app_flags.code = READ_STORAGE;
                    ble_app_flags.received_cmd = true;
                } else {
                    printk("[BLE_THREAD] Command ignored: busy\n");
                }
            } else if (event.conn == transfer.conn) {
                if (event.code == NONE) {
                    ble_app_flags.failed = true;
                    printk("[BLE_THREAD] Session disconnected\n");
                } else if (ble_app_flags.current_state == ble_app_wait_reply &&
                           event.seq == transfer.seq) {
                    /* Keep the first valid reply for this packet. */
                    if (!ble_app_flags.ack_received && !ble_app_flags.nack_received) {
                        ble_app_flags.ack_received = event.code == RECEIVED_ACK;
                        ble_app_flags.nack_received = event.code == RECEIVED_NACK;
                    }
                }
            }

            if (event.conn != NULL) {
                bt_conn_unref(event.conn);
            }
        }

        if (!ble_app_flags.failed ||
            ble_app_flags.current_state == ble_app_done) {
            fsm_handle_state();
        }
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
                          uint16_t *sequence)
{
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
    case READ_STORAGE:
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
            ble_app_flags.current_state = ble_app_idle;
            break;
        default:
            ble_app_flags.current_state = ble_app_idle;
            break;
        }
}
static void fsm_handle_state(void)
{
    switch (ble_app_flags.current_state) {
    case ble_app_idle:
        idle_handler();
        break;

    case ble_app_received_cmd:
        received_cmd_handler();
        break;

    case ble_app_handle_cmd:
        handle_cmd_handler();
        break;

    case ble_app_wait_reply:
        wait_reply_handler();
        break;

    case ble_app_done:
        done_handler();
        break;

    default:
        break;
    }
}

static void idle_handler(void){
    if (!ble_app_flags.received_cmd) {
        return;
    }

    ble_app_flags.verified_cmd = false;
    ble_app_flags.success_send = false;
    ble_app_flags.ack_received = false;
    ble_app_flags.nack_received = false;
    ble_app_flags.ack_timeout = false;
    ble_app_flags.retry_count = 0;
    ble_app_flags.done = false;
    ble_app_flags.failed = false;
}

static void received_cmd_handler(void)
{
    ble_app_flags.verified_cmd = false;

    if (ble_app_flags.code != READ_STORAGE) {
        return;
    }

    int err = storage_get_data_size(&transfer.storage_end);
    if (err != 0 || transfer.storage_end % STORAGE_BLOCK_SIZE != 0U) {
        printk("[BLE_THREAD] Invalid storage extent: %d\n", err);
        return;
    }

    transfer.page_offset = 0U;
    transfer.page_pos = STORAGE_BLOCK_DATA_SIZE;
    ble_app_flags.verified_cmd = true;
}

static void handle_cmd_handler(void)
{
    if (ble_app_flags.code != READ_STORAGE || transfer.conn == NULL ||
        bt_gatt_get_mtu(transfer.conn) < BLE_PACKET_SIZE + 3U) {
        printk("[BLE_THREAD] Cannot send a 244-byte packet\n");
        ble_app_flags.failed = true;
        return;
    }

    if (ble_app_flags.success_send) {
        // if retry_count is greater than max retries chaging the done state, else increase try count
        if (ble_app_flags.retry_count >= BLE_MAX_RETRIES) {
            ble_app_flags.failed = true;
            return;
        }
        ble_app_flags.retry_count++;
    } 
    else {
        // hadle case sened all datas of one page
        if (transfer.page_pos == STORAGE_BLOCK_DATA_SIZE) {
            // check if there is sending all datas in storage changing done state
            if (transfer.page_offset == transfer.storage_end) {
                ble_app_flags.done = true;
            } 
            else {
                /* Decode in place to avoid another 2046-byte payload buffer. */
                int err = decode_storage_data(transfer.page_offset, read_page);
                // err = 1 is page not data
                if (err == 1) {
                    transfer.page_offset += STORAGE_BLOCK_SIZE; // + 2048Byte
                    return; /* Skip the erased page, then process the next one. */
                }
                if (err != 0) {
                    ble_app_flags.failed = true;
                    return;
                }
                transfer.page_pos = 0U;
            }
        }
        /* Clear the entire packet to avoid sending stale bytes in unused payload space. */
        memset(transfer.packet, 0, sizeof(transfer.packet));
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
        // call encode_ble_packet to append CRC
        if (encode_ble_packet(transfer.packet) != 0) {
            ble_app_flags.failed = true;
            return;
        }
    }
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
        if (!ble_app_flags.done) {
            transfer.page_pos += transfer.payload_len;
            if (transfer.page_pos == STORAGE_BLOCK_DATA_SIZE) {
                transfer.page_offset += STORAGE_BLOCK_SIZE;
            }
        }

        transfer.seq++;
        ble_app_flags.success_send = false; /* Next handler prepares a new packet. */
        ble_app_flags.retry_count = 0U;
        ble_app_flags.nack_received = false;
        ble_app_flags.ack_timeout = false;
        return; /* Leave ACK set for fsm_change_state(). */
    }

    if (ble_app_flags.nack_received || k_uptime_get() >= transfer.deadline) {
        ble_app_flags.ack_timeout = !ble_app_flags.nack_received;
        if (ble_app_flags.retry_count >= BLE_MAX_RETRIES) {
            printk("[BLE_THREAD] Reply retry limit reached\n");
            ble_app_flags.failed = true;
        }
        /* Keep success_send, packet, sequence and page cursor for the retry. */
    }
}

/* NOTE REVIEW: Check connection-reference release and session reset on all exits. */
static void done_handler(void)
{
    if (transfer.conn != NULL) {
        bt_conn_unref(transfer.conn);
        transfer.conn = NULL;
    }

    /* NOTE REVIEW: Retire this sequence even after failure, before accepting another CMD. */
    transfer.seq++;
    transfer.storage_end = 0U;
    transfer.page_offset = 0U;
    transfer.page_pos = STORAGE_BLOCK_DATA_SIZE;
    transfer.payload_len = 0U;
    transfer.deadline = 0;

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

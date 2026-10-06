#include "ble_thread.h"
#include "nus.h"
#include <stdbool.h>
#include <zephyr/sys/printk.h>

#define BLE_MAX_RETRIES 5

/* Handle data written by the peer to the NUS RX characteristic. */
static void rx_received(struct bt_conn *conn, const uint8_t *data, uint16_t len);

static struct bt_nus_cb nus_callbacks = {.received = rx_received,
                                        .sent = NULL,
                                        .send_enabled = NULL,};
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

static struct ble_app_flags {
    uint8_t code; // Code indicating the type of command received
    bool received_cmd; // Flag to indicate if a command has been received
    bool verified_cmd; // Flag to indicate if the received command has been verified
    bool success_send; // Flag to indicate if the reply has been sent successfully
    uint8_t current_state; // Current state of the BLE application
    bool ack_received; // Flag to indicate if an ACK has been received
    bool nack_received; // Flag to indicate if a NACK has been received
    bool ack_timeout; // Flag to indicate if an ACK timeout has occurred
    uint8_t retry_count;
    bool  done; 
} ble_app_flags;

static void decode_command(const uint8_t *data, uint16_t len);
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
        printk("BLE initialization failed (err %d)\n", err);
    }
    printk("BLE initialization successful\n");
}

void ble_thread(void *p1, void *p2, void *p3)
{
    (void)p1;
    (void)p2;
    (void)p3;
    fsm_handle_state();
}

static void decode_command(const uint8_t *data, uint16_t len)
{
    if (len < 1) {
        return;
    }

    switch (data[0]) {
        case READ_STORAGE:
            ble_app_flags.code = READ_STORAGE;
            break;
        case REPLY_ACK:
            ble_app_flags.code = REPLY_ACK;
            break;
        case REPLY_NACK:
            ble_app_flags.code = REPLY_NACK;
            break;
        default:
            ble_app_flags.code = NONE;
            break;
    }
}

/* Handle data written by the peer to the NUS RX characteristic. */
static void rx_received(struct bt_conn *conn, const uint8_t *data, uint16_t len){
    ble_app_flags.received_cmd = true;
    decode_command(data, len);
}

static void fsm_change_state(void)
{
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

}
static void received_cmd_handler(void){

}
static void handle_cmd_handler(void){

}
static void wait_reply_handler(void){

}
static void done_handler(void){
    
}
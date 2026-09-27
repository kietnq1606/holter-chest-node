#include <zephyr/kernel.h>
#include "nus.h"

/* Handle data written by the peer to the NUS RX characteristic. */
static void rx_received(struct bt_conn *conn,
                        const uint8_t *data,
                        uint16_t len)
{
    static const uint8_t response[] = "Hi, I am Nordic";
    int err;

    printk("Received data: %.*s\n", (int)len, (const char *)data);

    if ((len != 4U) ||
        (data[0] != '2') ||
        (data[1] != '0') ||
        (data[2] != '0') ||
        (data[3] != '5')) {
        return;
    }

    err = bt_nus_send(conn, response, sizeof(response) - 1U);
    if (err) {
        printk("BLE response failed (err %d)\n", err);
    }
}
int main(void)
{
    int err;

    static struct bt_nus_cb nus_callbacks = {
        .received = rx_received,
        .sent = NULL,
        .send_enabled = NULL,
    };
    err = bt_init(&nus_callbacks);
    if (err) {
        printk("BLE initialization failed (err %d)\n", err);
        return err;
    }
    printk("BLE initialization successful\n");
    while (1) {
        k_sleep(K_SECONDS(1));
    }
    return 0;
}

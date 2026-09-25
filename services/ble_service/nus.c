#include <nus.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(nus);

static struct bt_nus_cb nus_cb;
/* Called when the peer changes its TX notification subscription. */
static void nus_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value);
/* Called when a TX notification has been transmitted. */
static void on_sent(struct bt_conn *conn, void *user_data);
/* Called when a peer writes data to NUS RX. */
static ssize_t on_receive(struct bt_conn *conn,
                        const struct bt_gatt_attr *attr,
                        const void *buf,
                        uint16_t len,
                        uint16_t offset,
                        uint8_t flags);

int bt_init(struct bt_nus_cb *callbacks){
    if (callbacks) {
		nus_cb.received = callbacks->received;
		nus_cb.sent = callbacks->sent;
		nus_cb.send_enabled = callbacks->send_enabled;
	}
    return 0;
}

/* Define and register the Nordic UART Service (NUS). */
BT_GATT_SERVICE_DEFINE(nus_svc,

    /* Identify this group of attributes as the NUS primary service. */
    BT_GATT_PRIMARY_SERVICE(BT_UUID_NUS_SERVICE),

    /* TX characteristic: the board sends data to the phone. */
    BT_GATT_CHARACTERISTIC(
        BT_UUID_NUS_TX,       /* UUID identifying NUS TX */
        BT_GATT_CHRC_NOTIFY,  /* Supports sending notifications */
        BT_GATT_PERM_READ,    /* Value attribute's read permission */
        NULL,                 /* No read callback */
        NULL,                 /* No write callback */
        NULL                  /* No user data */
    ),

    /*
     * CCC descriptor: the phone writes here to enable or disable
     * notifications from the TX characteristic.
     */
    BT_GATT_CCC(
        nus_ccc_cfg_changed,                  /* Subscription-change callback */
        BT_GATT_PERM_READ | BT_GATT_PERM_WRITE /* CCC access permissions */
    ),

    /* RX characteristic: the phone writes data to the board. */
    BT_GATT_CHARACTERISTIC(
        BT_UUID_NUS_RX,       /* UUID identifying NUS RX */
        BT_GATT_CHRC_WRITE |
        BT_GATT_CHRC_WRITE_WITHOUT_RESP, /* Accept either write type */
        BT_GATT_PERM_WRITE,   /* Permit writes to the value */
        NULL,                 /* No read callback */
        on_receive,           /* Called when the phone writes data */
        NULL                  /* No user data */
    )
);


int bt_send(struct bt_conn *conn, const uint8_t *data, uint16_t len){
    struct bt_gatt_notify_params params = {0};
    const struct bt_gatt_attr *attr = &nus_svc.attrs[2];
    /* Specify which characteristic sends the notification. */
    params.attr = attr;
    params.data = data;
    params.len = len;
    /* Called when the stack completes the send operation. */
    params.func = on_sent;
    if (!conn) {
        /*
        * No specific connection was supplied.
        * Send to all connected peers subscribed to TX notifications.
        */
        LOG_DBG("Notification send to all connected peers");
        return bt_gatt_notify_cb(NULL, &params);
    } else if (bt_gatt_is_subscribed(conn, attr, BT_GATT_CCC_NOTIFY)) {
        /* Send only to this connection if it subscribed. */
        return bt_gatt_notify_cb(conn, &params);
    } else {
        /* This connection has not subscribed to notifications. */
        return -EINVAL;
    }
}

//callback
static void nus_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	if (nus_cb.send_enabled) {
		LOG_DBG("Notification has been turned %s",
			value == BT_GATT_CCC_NOTIFY ? "on" : "off");
		nus_cb.send_enabled(value == BT_GATT_CCC_NOTIFY ?
			BT_NUS_SEND_STATUS_ENABLED : BT_NUS_SEND_STATUS_DISABLED);
	}
}

static void on_sent(struct bt_conn *conn, void *user_data)
{
	ARG_UNUSED(user_data);

	LOG_DBG("Data send, conn %p", (void *)conn);

	if (nus_cb.sent) {
		nus_cb.sent(conn);
	}
}

static ssize_t on_receive(struct bt_conn *conn,
                        const struct bt_gatt_attr *attr,
                        const void *buf,
                        uint16_t len,
                        uint16_t offset,
                        uint8_t flags)
{
	ARG_UNUSED(offset);
	ARG_UNUSED(flags);

	LOG_DBG("Received data, handle %d, conn %p",
		attr->handle, (void *)conn);

	if (nus_cb.received) {
		nus_cb.received(conn, buf, len);
    }      
	return len;
}

static void rx_received(struct bt_conn *conn,
                        const uint8_t *data,
                        uint16_t len)
{
    /* Parse or copy received data here. */
}

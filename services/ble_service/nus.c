#include <nus.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(nus);


/* Data sent in every advertising packet. */
static const struct bt_data advertising_data[] = {
    /* Advertise as a discoverable Bluetooth LE-only device. */
    BT_DATA_BYTES(
        BT_DATA_FLAGS,                         /* Advertising flags field. */
        BT_LE_AD_GENERAL |                    /* Discoverable without a timeout. */
        BT_LE_AD_NO_BREDR                     /* Bluetooth LE only, no BR/EDR. */
    ),

    /* Allow scanners to identify the custom NUS service. */
    BT_DATA_BYTES(
        BT_DATA_UUID128_ALL,                  /* Complete list of 128-bit service UUIDs. */
        BT_UUID_NUS_VAL                       /* Custom NUS service UUID bytes. */
    ),
};

/* Extra data returned when a scanner requests more information. */
static const struct bt_data scan_response_data[] = {
    /* Keep the complete device name out of the main advertising packet. */
    BT_DATA(
        BT_DATA_NAME_COMPLETE,                /* Complete local device name field. */
        CONFIG_BT_DEVICE_NAME,                /* Name configured in prj.conf. */
        sizeof(CONFIG_BT_DEVICE_NAME) - 1     /* String length without the null byte. */
    ),
};

static struct bt_nus_cb nus_cb;
/* Called when a connection attempt completes. */
static void connected(struct bt_conn *conn, uint8_t err);
/* Called when an active connection terminates. */
static void disconnected(struct bt_conn *conn, uint8_t reason);
/* Called after a connection object is released. */
static void recycled(void);
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

/* Initialize Bluetooth and start connectable advertising. */
int bt_init(struct bt_nus_cb *callbacks)
{
    int err;

    LOG_INF("Initializing BLE...");

    /* Register application callbacks before accepting peer connections. */
    if (callbacks) {
        nus_cb = *callbacks;
    } else {
        /* Disable all optional callbacks when none are supplied. */
        nus_cb = (struct bt_nus_cb){0};
    }

    /* Initialize the Bluetooth Host and Controller synchronously. */
    err = bt_enable(NULL);
    if (err) {
        LOG_ERR("BLE initialization failed (err %d)", err);
        return err;
    }

    /* Start connectable advertising with service data and scan response data. */
    err = bt_le_adv_start(
        BT_LE_ADV_CONN_FAST_1,                  /* Fast connectable advertising parameters. */
        advertising_data,                     /* Primary advertising data. */
        ARRAY_SIZE(advertising_data),          /* Number of advertising data entries. */
        scan_response_data,                    /* Data returned to active scanners. */
        ARRAY_SIZE(scan_response_data)         /* Number of scan response entries. */
    );
    if (err) {
        LOG_ERR("BLE advertising failed (err %d)", err);
        return err;
    }

    LOG_INF("BLE initialized and advertising started");
    return 0;
}

/* Register connection callbacks with the Bluetooth Host. */
BT_CONN_CB_DEFINE(conn_callbacks) = {
    .connected = connected,
    .disconnected = disconnected,
    .recycled = recycled,
};

/* Define and register the Nordic UART Service (NUS). */
BT_GATT_SERVICE_DEFINE(nus_svc,

    /* Identify this group of attributes as the NUS primary service. */
    BT_GATT_PRIMARY_SERVICE(BT_UUID_NUS_SERVICE),

    /* TX characteristic: the board sends data to the phone. */
    BT_GATT_CHARACTERISTIC(
        BT_UUID_NUS_TX,       /* UUID identifying NUS TX */
        BT_GATT_CHRC_NOTIFY,  /* Supports sending notifications */
        BT_GATT_PERM_NONE,    /* TX value is available only through notifications. */
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


/* Send NUS TX data to one peer or all subscribed peers. */
int bt_nus_send(struct bt_conn *conn, const uint8_t *data, uint16_t len){
    struct bt_gatt_notify_params params = {0};
    /*
    attrs[0] = Primary Service
    attrs[1] = TX Characteristic Declaration
    attrs[2] = TX Characteristic Value
    attrs[3] = CCC
    attrs[4] = RX Characteristic Declaration
    attrs[5] = RX Characteristic Value
     */
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

/* Forward a successful connection event to the application callback. */
static void connected(struct bt_conn *conn, uint8_t err){
    if (err) {
        LOG_ERR("Connection failed (err %u)", err);
        return;
    }
    else {
        LOG_INF("Connected");
    }

    if (nus_cb.connected) {
        nus_cb.connected(conn);
    }
}
/* Report a disconnection and restart connectable advertising. */
static void disconnected(struct bt_conn *conn, uint8_t reason)
{
    LOG_INF("Disconnected (reason 0x%02x)", reason);
    if (nus_cb.disconnected) {
        nus_cb.disconnected(conn, reason);
    }
}
static void recycled(void)
{
    int err;
    err = bt_le_adv_start(
        BT_LE_ADV_CONN_FAST_1,
        advertising_data,
        ARRAY_SIZE(advertising_data),
        scan_response_data,
        ARRAY_SIZE(scan_response_data)
    );
    if (err) {
        LOG_ERR("Advertising restart failed (err %d)", err);
        return;
    }
    LOG_INF("Advertising restarted");
}
/* Forward the TX notification subscription state to the application. */
static void nus_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	if (nus_cb.send_enabled) {
		LOG_DBG("Notification has been turned %s",
			value == BT_GATT_CCC_NOTIFY ? "on" : "off");
		nus_cb.send_enabled(value == BT_GATT_CCC_NOTIFY ?
			BT_NUS_SEND_STATUS_ENABLED : BT_NUS_SEND_STATUS_DISABLED);
	}
}

/* Notify the application after a TX notification is processed. */
static void on_sent(struct bt_conn *conn, void *user_data)
{
	ARG_UNUSED(user_data);

	LOG_DBG("Data sent, conn %p", (void *)conn);

	if (nus_cb.sent) {
		nus_cb.sent(conn);
	}
}

/* Forward data written to the RX characteristic to the application. */
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


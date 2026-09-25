/*
 * Copyright (c) 2018 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#ifndef BT_NUS_H_
#define BT_NUS_H_


#include <zephyr/types.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>

#ifdef __cplusplus
extern "C" {
#endif


/** @brief UUID of the NUS Service. **/
#define BT_UUID_NUS_VAL \
	BT_UUID_128_ENCODE(0x6e400001, 0xb5a3, 0xf393, 0xe0a9, 0xe50e24d3ca9e)

/** @brief UUID of the TX Characteristic. **/
#define BT_UUID_NUS_TX_VAL \
	BT_UUID_128_ENCODE(0x6e400003, 0xb5a3, 0xf393, 0xe0a9, 0xe51e24dcca9e)

/** @brief UUID of the RX Characteristic. **/
#define BT_UUID_NUS_RX_VAL \
	BT_UUID_128_ENCODE(0x6e400002, 0xb5a3, 0xf393, 0xe0a9, 0xe51e24dcca9e)

#define BT_UUID_NUS_SERVICE   BT_UUID_DECLARE_128(BT_UUID_NUS_VAL)
#define BT_UUID_NUS_RX        BT_UUID_DECLARE_128(BT_UUID_NUS_RX_VAL)
#define BT_UUID_NUS_TX        BT_UUID_DECLARE_128(BT_UUID_NUS_TX_VAL)

/*NUS send status. */
enum bt_nus_send_status {
	/** Send notification enabled. */
	BT_NUS_SEND_STATUS_ENABLED,
	/** Send notification disabled. */
	BT_NUS_SEND_STATUS_DISABLED,
};

struct bt_nus_cb {
	//The data has been received as a write request on the NUS RX Characteristic.
	void (*received)(struct bt_conn *conn, const uint8_t *const data, uint16_t len);
    //The data has been sent as a notification and written on the NUS TX Characteristic.
	void (*sent)(struct bt_conn *conn);
    //Indicate the CCCD descriptor status of the NUS TX characteristic.
	void (*send_enabled)(enum bt_nus_send_status status);

};

int bt_init(struct bt_nus_cb *callbacks);
int bt_send(struct bt_conn *conn, const uint8_t *data, uint16_t len);



#ifdef __cplusplus
}
#endif

/**
 *@}
 */

#endif /* BT_NUS_H_ */

#ifndef TRACKING_SYSTEM_H_
#define TRACKING_SYSTEM_H_

#include <stdint.h>
#include <zephyr/kernel.h>

/* Set to 0 for production builds to strip all tracking code and RAM. */
#ifndef TRACKING_ENABLE
#define TRACKING_ENABLE 1
#endif

#if TRACKING_ENABLE

/* Initialize atomic counters and bind shared queues before threads start. */
void tracking_system_init(struct k_msgq *free_q, struct k_msgq *ready_q, uint32_t initial_flash_bytes);

/* Emit BOOT_OK event and start the periodic 1-second heartbeat work item. */
void tracking_system_start(void);

/* Lock-free updates safe to call from acquisition, storage, or BLE threads. */
void tracking_set_acq_state(uint8_t state);
void tracking_inc_acq_pkt(void);
void tracking_inc_acq_drop(void);

void tracking_set_stg_state(uint8_t state);
void tracking_inc_stg_page(uint32_t flash_bytes);
void tracking_inc_stg_err(void);
void tracking_set_flash_bytes(uint32_t flash_bytes);

void tracking_set_ble_state(uint8_t state);
void tracking_add_ble_tx(uint32_t bytes);
void tracking_inc_ble_err(void);

/* Emit an immediate structured event line: TRK_EVT,ms=...,src=...,lvl=...,msg=...,val=... */
void tracking_event(const char *src, const char *lvl, const char *msg, int32_t val);

/* Macro wrappers allow zero-cost compile-time removal when TRACKING_ENABLE == 0. */
#define TRK_INIT(free_q, ready_q, init_bytes) tracking_system_init((free_q), (ready_q), (init_bytes))
#define TRK_START()                           tracking_system_start()
#define TRK_SET_ACQ_STATE(st)                 tracking_set_acq_state((uint8_t)(st))
#define TRK_INC_ACQ_PKT()                     tracking_inc_acq_pkt()
#define TRK_INC_ACQ_DROP()                    tracking_inc_acq_drop()
#define TRK_SET_STG_STATE(st)                 tracking_set_stg_state((uint8_t)(st))
#define TRK_INC_STG_PAGE(flash_b)             tracking_inc_stg_page((uint32_t)(flash_b))
#define TRK_INC_STG_ERR()                     tracking_inc_stg_err()
#define TRK_SET_FLASH_BYTES(flash_b)          tracking_set_flash_bytes((uint32_t)(flash_b))
#define TRK_SET_BLE_STATE(st)                 tracking_set_ble_state((uint8_t)(st))
#define TRK_ADD_BLE_TX(bytes)                 tracking_add_ble_tx((uint32_t)(bytes))
#define TRK_INC_BLE_ERR()                     tracking_inc_ble_err()
#define TRK_EVENT(src, lvl, msg, val)         tracking_event((src), (lvl), (msg), (int32_t)(val))

#else

/* No-op stubs when tracking is disabled for deployment. */
#define TRK_INIT(free_q, ready_q, init_bytes) ((void)0)
#define TRK_START()                           ((void)0)
#define TRK_SET_ACQ_STATE(st)                 ((void)0)
#define TRK_INC_ACQ_PKT()                     ((void)0)
#define TRK_INC_ACQ_DROP()                    ((void)0)
#define TRK_SET_STG_STATE(st)                 ((void)0)
#define TRK_INC_STG_PAGE(flash_b)             ((void)0)
#define TRK_INC_STG_ERR()                     ((void)0)
#define TRK_SET_FLASH_BYTES(flash_b)          ((void)0)
#define TRK_SET_BLE_STATE(st)                 ((void)0)
#define TRK_ADD_BLE_TX(bytes)                 ((void)0)
#define TRK_INC_BLE_ERR()                     ((void)0)
#define TRK_EVENT(src, lvl, msg, val)         ((void)0)

#endif /* TRACKING_ENABLE */

#endif /* TRACKING_SYSTEM_H_ */

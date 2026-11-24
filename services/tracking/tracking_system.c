#include "tracking_system.h"

#if TRACKING_ENABLE

#include <zephyr/sys/atomic.h>
#include <zephyr/sys/printk.h>

/* Heartbeat interval; kept at 1 s to avoid saturating USB CDC console. */
#define TRACKING_PERIOD_MS 1000U

/* Atomics let worker threads update metrics without taking a mutex. */
static struct {
	struct k_msgq *free_q;   /* Shared acquisition free buffer pool. */
	struct k_msgq *ready_q;  /* Filled buffers waiting for storage_thread. */
	atomic_t acq_state;      /* Current acq_thread FSM state enum. */
	atomic_t acq_pkts;       /* Completed 114-byte sensor packets since boot. */
	atomic_t acq_drops;      /* Count of DMA pauses caused by empty free_q. */
	atomic_t stg_state;      /* Current storage_thread FSM state enum. */
	atomic_t stg_pages;      /* Flash pages committed since boot. */
	atomic_t stg_errs;       /* Flash space-check (-ENOSPC) or write errors. */
	atomic_t ble_state;      /* Current ble_thread FSM state enum. */
	atomic_t ble_tx_bytes;   /* ACKed BLE NUS payload bytes sent since boot. */
	atomic_t ble_errs;       /* BLE NACK, ACK timeout, or erase errors. */
	atomic_t flash_bytes;    /* Committed Flash data extent in bytes. */
} trk;

/* Runs on Zephyr's system workqueue so tracking needs no dedicated thread stack. */
static struct k_work_delayable trk_work;

/* Print one parseable TRK_STAT heartbeat line and reschedule for the next second. */
static void trk_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	uint32_t q_free = trk.free_q ? k_msgq_num_used_get(trk.free_q) : 0U;
	uint32_t q_ready = trk.ready_q ? k_msgq_num_used_get(trk.ready_q) : 0U;

	/* Integer-only formatting avoids pulling floating-point printf support. */
	printk("TRK_STAT,ms=%u,acq_st=%u,acq_pkt=%u,acq_drop=%u,"
	       "stg_st=%u,stg_pg=%u,stg_err=%u,"
	       "ble_st=%u,ble_tx=%u,ble_err=%u,"
	       "q_free=%u,q_ready=%u,flash_b=%u\n",
	       (unsigned int)k_uptime_get_32(),
	       (unsigned int)atomic_get(&trk.acq_state),
	       (unsigned int)atomic_get(&trk.acq_pkts),
	       (unsigned int)atomic_get(&trk.acq_drops),
	       (unsigned int)atomic_get(&trk.stg_state),
	       (unsigned int)atomic_get(&trk.stg_pages),
	       (unsigned int)atomic_get(&trk.stg_errs),
	       (unsigned int)atomic_get(&trk.ble_state),
	       (unsigned int)atomic_get(&trk.ble_tx_bytes),
	       (unsigned int)atomic_get(&trk.ble_errs),
	       (unsigned int)q_free,
	       (unsigned int)q_ready,
	       (unsigned int)atomic_get(&trk.flash_bytes));

	(void)k_work_reschedule(&trk_work, K_MSEC(TRACKING_PERIOD_MS));
}

void tracking_system_init(struct k_msgq *free_q, struct k_msgq *ready_q, uint32_t initial_flash_bytes)
{
	trk.free_q = free_q;
	trk.ready_q = ready_q;
	atomic_set(&trk.acq_state, 0);
	atomic_set(&trk.acq_pkts, 0);
	atomic_set(&trk.acq_drops, 0);
	atomic_set(&trk.stg_state, 0);
	atomic_set(&trk.stg_pages, 0);
	atomic_set(&trk.stg_errs, 0);
	atomic_set(&trk.ble_state, 0);
	atomic_set(&trk.ble_tx_bytes, 0);
	atomic_set(&trk.ble_errs, 0);
	atomic_set(&trk.flash_bytes, (atomic_val_t)initial_flash_bytes);
	k_work_init_delayable(&trk_work, trk_work_handler);
}

void tracking_system_start(void)
{
	TRK_EVENT("SYS", "INF", "BOOT_OK", 0);
	(void)k_work_reschedule(&trk_work, K_MSEC(TRACKING_PERIOD_MS));
}

void tracking_set_acq_state(uint8_t state)
{
	atomic_set(&trk.acq_state, (atomic_val_t)state);
}

void tracking_inc_acq_pkt(void)
{
	atomic_inc(&trk.acq_pkts);
}

void tracking_inc_acq_drop(void)
{
	atomic_inc(&trk.acq_drops);
}

void tracking_set_stg_state(uint8_t state)
{
	atomic_set(&trk.stg_state, (atomic_val_t)state);
}

void tracking_inc_stg_page(uint32_t flash_bytes)
{
	atomic_inc(&trk.stg_pages);
	atomic_set(&trk.flash_bytes, (atomic_val_t)flash_bytes);
}

void tracking_inc_stg_err(void)
{
	atomic_inc(&trk.stg_errs);
}

void tracking_set_flash_bytes(uint32_t flash_bytes)
{
	atomic_set(&trk.flash_bytes, (atomic_val_t)flash_bytes);
}

void tracking_set_ble_state(uint8_t state)
{
	atomic_set(&trk.ble_state, (atomic_val_t)state);
}

void tracking_add_ble_tx(uint32_t bytes)
{
	atomic_add(&trk.ble_tx_bytes, (atomic_val_t)bytes);
}

void tracking_inc_ble_err(void)
{
	atomic_inc(&trk.ble_errs);
}

/* Emit discrete state/error events parsed by tracking_sys.py timeline. */
void tracking_event(const char *src, const char *lvl, const char *msg, int32_t val)
{
	printk("TRK_EVT,ms=%u,src=%s,lvl=%s,msg=%s,val=%d\n",
	       (unsigned int)k_uptime_get_32(), src, lvl, msg, (int)val);
}

#endif /* TRACKING_ENABLE */

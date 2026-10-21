#include "storage_thread.h"
#include <errno.h>
#include <stdbool.h>
#include <zephyr/kernel.h>

enum storage_state {
    STORAGE_IDLE = 0, /* Wait for a filled acquisition buffer. */
    STORAGE_WAIT_WRITE_READY, /* Wait for exclusive access and one free page. */
    STORAGE_WRITE_DATA, /* Write and commit while holding the mutex. */
    STORAGE_DONE, /* Return the committed buffer for reuse. */
};

/* Only the storage thread updates these flags after initialization. */
static struct {
    uint8_t current_state; /* Selects the handler and its transition rules. */
    bool has_active_buffer; /* A buffer has been taken from ready_queue. */
    bool storage_ready; /* Space is available and this thread owns the mutex. */
    bool done; /* The write committed successfully and the mutex was released. */
    bool failed; /* Retain a failed write without reprogramming a partial page. */
} storage_app_flags;

static struct {
    struct k_msgq *free_queue; /* Buffer pointers available to acquisition. */
    struct k_msgq *ready_queue; /* Filled buffer pointers handed over by acquisition. */
    struct k_mutex *storage_lock; /* Serializes Flash and metadata access with BLE. */
    uint8_t *active_buffer; /* Owned until committed and returned to free_queue. */
    bool storage_locked; /* Retry a failed unlock without repeating the write. */
} storage_thread_resources;

static void fsm_change_state(void);
static void fsm_handle_state(void);
static void storage_idle_handler(void);
static void storage_wait_write_ready_handler(void);
static void storage_write_data_handler(void);
static void storage_done_handler(void);


/* Initialize the service and FSM once, before starting the worker threads. */
int storage_thread_init(void){
    /* Recover storage before any worker can access Flash. */
    int err = storage_init();
    if (err != 0) {
        return err;
    }

    storage_app_flags.current_state = STORAGE_IDLE;
    storage_app_flags.has_active_buffer = false;
    storage_app_flags.storage_ready = false;
    storage_app_flags.done = false;
    storage_app_flags.failed = false;

    storage_thread_resources.active_buffer = NULL;
    storage_thread_resources.storage_locked = false;

    return 0;
}

/* Run one action and transition per iteration; handlers perform the waits. */
void storage_thread(void *p1, void *p2, void *p3)
{
    /* main.c passes the mutex, free queue, then ready queue. */
    storage_thread_resources.storage_lock = p1;
    storage_thread_resources.free_queue = p2;
    storage_thread_resources.ready_queue = p3;
    while (1) {
        fsm_handle_state();
        fsm_change_state();
    }
}

/* Change state using the flags produced by the last handler. */
static void fsm_change_state(void){
    /* Handlers finish their actions before transitions are evaluated. */
    switch (storage_app_flags.current_state) {
        case STORAGE_IDLE:
            storage_app_flags.current_state = storage_app_flags.has_active_buffer
                ? STORAGE_WAIT_WRITE_READY : storage_app_flags.current_state;
            break;
        case STORAGE_WAIT_WRITE_READY:
            storage_app_flags.current_state = storage_app_flags.storage_ready
                ? STORAGE_WRITE_DATA : storage_app_flags.current_state;
            break;

        case STORAGE_WRITE_DATA:
            storage_app_flags.current_state = storage_app_flags.done &&
                                             !storage_thread_resources.storage_locked
                ? STORAGE_DONE : storage_app_flags.current_state;
            break;

        case STORAGE_DONE:
            if (!storage_app_flags.has_active_buffer) {
                storage_app_flags.current_state = STORAGE_IDLE;
            }
            break;
        default:
            break;
    }
}
/* Dispatch the current action without changing the state itself. */
static void fsm_handle_state(void){
        switch (storage_app_flags.current_state) {
        case STORAGE_IDLE:
            storage_idle_handler();
            break;
        case STORAGE_WAIT_WRITE_READY:
            storage_wait_write_ready_handler();
            break;
        case STORAGE_WRITE_DATA:
            storage_write_data_handler();
            break;

        case STORAGE_DONE:
            storage_done_handler();
            break;
        default:
            break;
    }
}

/* Take ownership of the next buffer; no Flash access is needed here. */
static void storage_idle_handler(void){
    /* Block without polling; acquisition transfers ownership through the queue. */
    int err = k_msgq_get(storage_thread_resources.ready_queue,
            &storage_thread_resources.active_buffer,
            K_FOREVER);
    if (err != 0) {
        printk("Storage: get buffer failed (%d)\n", err);
        return;
    }
    storage_app_flags.has_active_buffer = (storage_thread_resources.active_buffer != NULL);
}
/* Hold the mutex only if a complete page can be written. */
static void storage_wait_write_ready_handler(void)
{
    storage_app_flags.storage_ready = false;

    /* Wait for BLE and serialize the space check with writes and erases. */
    int err;

    if (storage_thread_resources.storage_locked) {
        err = k_mutex_unlock(storage_thread_resources.storage_lock);
        if (err != 0) {
            printk("Storage: unlock failed (%d)\n", err);
            return;
        }
        storage_thread_resources.storage_locked = false;
    }

    err = k_mutex_lock(storage_thread_resources.storage_lock, K_FOREVER);
    if (err != 0) {
        printk("Storage: lock failed (%d)\n", err);
        return;
    }
    storage_thread_resources.storage_locked = true;
    err = storage_check_write_space();
    if (err != 0) {
        /* BLE needs this mutex to erase storage and make room. */
        int unlock_err = k_mutex_unlock(storage_thread_resources.storage_lock);
        if (unlock_err != 0) {
            printk("Storage: space check failed (%d), unlock failed (%d)\n",
                   err, unlock_err);
            return;
        }
        storage_thread_resources.storage_locked = false;
        if (err == -ENOSPC){
            printk("Storage: no write space\n");
            /* Keep the pending buffer and avoid a busy loop while full. */
            k_sleep(K_MSEC(100));
        }
        else {
            printk("Storage: space check failed (%d)\n", err);
        }
        return;
    }
    /* Keep the mutex across the transition to prevent an intervening erase. */
    storage_app_flags.storage_ready = true;
}
/* Attempt each write once; keep the worker alive and retain failed data. */
static void storage_write_data_handler(void){
    if (!storage_app_flags.done && !storage_app_flags.failed) {
        /* Do not repeat a write after an unlock error or partial program. */
        int err = storage_write(storage_thread_resources.active_buffer,
                                STORAGE_BLOCK_DATA_SIZE);
        storage_app_flags.done = (err == 0);
        storage_app_flags.failed = (err != 0);

        if (err != 0) {
            printk("Storage: write failed (%d)\n", err);
        }
    }
    /* Release after each write, including failures, so BLE can acquire it. */
    if (storage_thread_resources.storage_locked) {
        int err = k_mutex_unlock(storage_thread_resources.storage_lock);
        if (err != 0) {
            printk("Storage: unlock failed (%d)\n", err);
            return;
        }
        storage_thread_resources.storage_locked = false;
    }
    storage_app_flags.storage_ready = false;
    if (storage_app_flags.failed) {
        /* Recovery is required; never overwrite or recycle uncommitted data. */
        k_sleep(K_MSEC(100));
        return;
    }
}

/* Transfer ownership back to acquisition and clear the completed cycle flags. */
static void storage_done_handler(void)
{
    /* Return only committed data; acquisition overwrites it, so no clearing is needed. */
    int err = k_msgq_put(storage_thread_resources.free_queue,
                   &storage_thread_resources.active_buffer,
                   K_FOREVER);
    if (err != 0) {
        printk("Storage: return buffer failed (%d)\n", err);
        return;
    }
    storage_thread_resources.active_buffer = NULL;
    storage_app_flags.done = false;
    storage_app_flags.has_active_buffer = false;
}

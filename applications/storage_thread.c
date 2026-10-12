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
    bool failed; /* A fatal error ends the thread without returning its buffer. */
} storage_app_flags;

static struct {
    struct k_msgq *free_queue; /* Buffer pointers available to acquisition. */
    struct k_msgq *ready_queue; /* Filled buffer pointers handed over by acquisition. */
    struct k_mutex *storage_lock; /* Serializes Flash and metadata access with BLE. */
    uint8_t *active_buffer; /* Owned until committed and returned to free_queue. */
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
        if (storage_app_flags.failed) {
            /* Stop on error without returning uncommitted data for reuse. */
            return;
        }
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
            storage_app_flags.current_state = storage_app_flags.done
                ? STORAGE_DONE : storage_app_flags.current_state;
            break;

        case STORAGE_DONE:
            storage_app_flags.current_state = STORAGE_IDLE;
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
    k_msgq_get(storage_thread_resources.ready_queue,
            &storage_thread_resources.active_buffer,
            K_FOREVER);
    storage_app_flags.has_active_buffer = (storage_thread_resources.active_buffer != NULL);
}
/* Hold the mutex only if a complete page can be written. */
static void storage_wait_write_ready_handler(void)
{
    storage_app_flags.storage_ready = false;

    /* Wait for BLE and serialize the space check with writes and erases. */
    k_mutex_lock(storage_thread_resources.storage_lock, K_FOREVER);
    int err = storage_check_write_space();
    if (err != 0) {
        /* BLE needs this mutex to erase storage and make room. */
        k_mutex_unlock(storage_thread_resources.storage_lock);
        if (err == -ENOSPC){
            /* Keep the pending buffer and avoid a busy loop while full. */
            k_sleep(K_MSEC(100));
        }
        else {
            printk("Storage: space check failed (%d)\n", err);
            storage_app_flags.failed = true;
        }
        return;
    }
    /* Keep the mutex across the transition to prevent an intervening erase. */
    storage_app_flags.storage_ready = true;
}
/* Attempt one write; failures retain the buffer and stop further processing. */
static void storage_write_data_handler(void){
    /* One synchronous write includes data verification and metadata commit. */
    int err = storage_write(storage_thread_resources.active_buffer,
                            STORAGE_BLOCK_DATA_SIZE);
    if (err != 0) {
        printk("Storage: write failed (%d)\n", err);
        storage_app_flags.failed = true;
    }
    /* Release after each write, including failures, so BLE can acquire it. */
    if (k_mutex_unlock(storage_thread_resources.storage_lock) != 0) {
        printk("Storage: unlock failed\n");
        storage_app_flags.failed = true;
        return;
    }
    storage_app_flags.storage_ready = false;
    storage_app_flags.done = !storage_app_flags.failed;
}

/* Transfer ownership back to acquisition and clear the completed cycle flags. */
static void storage_done_handler(void)
{
    /* Return only committed data; acquisition overwrites it, so no clearing is needed. */
    if (k_msgq_put(storage_thread_resources.free_queue,
                   &storage_thread_resources.active_buffer,
                   K_FOREVER) == 0) {
        storage_thread_resources.active_buffer = NULL;
        storage_app_flags.done = false;
        storage_app_flags.has_active_buffer = false;
    }
}

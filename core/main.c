#include <zephyr/kernel.h>
#include "acq_thread.h"
#include "storage_thread.h"
#include "ble_thread.h"

#define ACQ_STACK_SIZE     1024
#define STORAGE_STACK_SIZE 1536
#define BLE_STACK_SIZE     2048

/* Lower values have higher priority. */
#define ACQ_PRIORITY       4
#define STORAGE_PRIORITY   6
#define BLE_APP_PRIORITY   5

/* Allocate a separate static stack for each thread. */
K_THREAD_STACK_DEFINE(acq_stack, ACQ_STACK_SIZE);
K_THREAD_STACK_DEFINE(storage_stack, STORAGE_STACK_SIZE);
K_THREAD_STACK_DEFINE(ble_stack, BLE_STACK_SIZE);

K_MUTEX_DEFINE(storage_lock);

// buffer (2046Bytes) use to save data of ad8232 and bmi323
static uint8_t buffer_a[STORAGE_BLOCK_DATA_SIZE];
static uint8_t buffer_b[STORAGE_BLOCK_DATA_SIZE];
// queue use for writign data into buffer
K_MSGQ_DEFINE(free_queue, sizeof(uint8_t *), 2, __alignof__(uint8_t *));
// queue use for reading data from buffer to write into storage
K_MSGQ_DEFINE(ready_queue, sizeof(uint8_t *), 2, __alignof__(uint8_t *));

/* Kernel thread control blocks. */
static struct k_thread acq_tcb;
static struct k_thread storage_tcb;
static struct k_thread ble_tcb;

int main(void)
{
    int err;

    /* Initialize shared resources before starting threads. */
    ble_thread_init();
    err = storage_thread_init();
    if (err != 0) {
        return err;
    }

    err = acquisition_init();
    if (err != 0) {
        return err;
    }

    /* storage_thread */
    k_thread_create(
        &storage_tcb,
        storage_stack,
        K_THREAD_STACK_SIZEOF(storage_stack),
        storage_thread, 
        &storage_lock, &free_queue, &ready_queue,
        STORAGE_PRIORITY,
        0,
        K_FOREVER
    );

    /* acquisition_thread */
    k_thread_create(
        &acq_tcb,
        acq_stack,
        K_THREAD_STACK_SIZEOF(acq_stack),
        acquisition_thread,
        &free_queue, &ready_queue, NULL,
        ACQ_PRIORITY,
        0,
        K_FOREVER
    );

    k_thread_create(
        &ble_tcb,
        ble_stack,
        K_THREAD_STACK_SIZEOF(ble_stack),
        ble_thread,
        &storage_lock, NULL, NULL,
        BLE_APP_PRIORITY,
        0,
        K_FOREVER
    );

    /* Start order does not guarantee initialization completion. */
    k_thread_start(&storage_tcb);
    /* Acquisition must wait for START and storage readiness. */
    k_thread_start(&acq_tcb);
    k_thread_start(&ble_tcb);

    /* Worker threads continue after main exits. */
    return 0;
}
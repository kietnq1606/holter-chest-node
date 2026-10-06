#include "storage_thread.h"

#include <errno.h>

/* TODO: Declare thread stack, control block and a bounded request queue. */

int storage_thread_init(void)
{
    /* TODO: Initialize the request queue before starting the thread. */
    return -ENOSYS;
}

int storage_thread_submit_write(const uint8_t *buffer, size_t length)
{
    (void)buffer;
    (void)length;

    /* TODO: Validate and queue a WRITE descriptor without copying sample data. */
    return -ENOSYS;
}

int storage_thread_submit_read(uint32_t offset, uint8_t *buffer, size_t length)
{
    (void)offset;
    (void)buffer;
    (void)length;

    /* TODO: Validate and queue a bounded READ request. */
    return -ENOSYS;
}

int storage_thread_submit_flush(void)
{
    /* TODO: Queue a barrier after acquisition has handed off its final block. */
    return -ENOSYS;
}

void storage_thread_entry(void *p1, void *p2, void *p3)
{
    (void)p1;
    (void)p2;
    (void)p3;

    /* TODO: Initialize/recover storage here and report readiness or failure. */
    /* TODO: Wait for requests, call the storage service and report each result. */
    /* TODO: Report flush success only after data and metadata are committed. */
}

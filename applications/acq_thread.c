#include <zephyr/kernel.h>
#include "acquisition.h"

#include <errno.h>

/* TODO: Declare thread stack, control block and sampling state. */

int acquisition_init(void)
{
    /* TODO: Initialize sampling and the fixed buffer pool. */
    return -ENOSYS;
}

int acquisition_start(void)
{
    /* TODO: Queue START without changing state from the caller. */
    return -ENOSYS;
}

int acquisition_stop(void)
{
    /* TODO: Queue STOP; report completion after the final block handoff. */
    return -ENOSYS;
}

void acquisition_thread_entry(void *p1, void *p2, void *p3)
{
    (void)p1;
    (void)p2;
    (void)p3;

    /* TODO: Wait for commands or sampling events, then read and frame samples. */
    /* TODO: Submit blocks without waiting; handle pool or queue exhaustion. */
}

#include <zephyr/kernel.h>
#include "acquisition.h"

#include <errno.h>


int acquisition_init(void)
{
    
    return -ENOSYS;
}

void acquisition_thread(void *p1, void *p2, void *p3)
{
    (void)p1;
    (void)p2;
    (void)p3;

    /* TODO: Wait for commands or sampling events, then read and frame samples. */
    /* TODO: Submit blocks without waiting; handle pool or queue exhaustion. */
}

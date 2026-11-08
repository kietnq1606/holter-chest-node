#ifndef APPLICATIONS_STORAGE_THREAD_H_
#define APPLICATIONS_STORAGE_THREAD_H_

#include <stddef.h>
#include <stdint.h>
#include "storage.h"

/* Initialize before starting the thread. */
int storage_thread_init(void);

/* Sole application context for storage service calls. */
void storage_thread(void *p1, void *p2, void *p3);

#endif
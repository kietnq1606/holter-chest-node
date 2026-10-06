#ifndef APPLICATIONS_STORAGE_THREAD_H_
#define APPLICATIONS_STORAGE_THREAD_H_

#include <stddef.h>
#include <stdint.h>

/* Initialize before starting the thread. */
int storage_thread_init(void);

int storage_thread_submit_write(const uint8_t *buffer, size_t length);

/* Sole application context for storage service calls. */
void storage_thread(void *p1, void *p2, void *p3);

#endif

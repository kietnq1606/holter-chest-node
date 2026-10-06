#ifndef APPLICATIONS_BLE_THREAD_H_
#define APPLICATIONS_BLE_THREAD_H_

#include <stddef.h>
#include <stdint.h>

/* Initialize before entering the application event loop. */
void ble_thread_init(void);

/* May run in main to reuse its stack; callbacks only queue events. */
void ble_thread(void *p1, void *p2, void *p3);

#endif

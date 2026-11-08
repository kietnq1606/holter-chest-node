#ifndef APPLICATIONS_ACQUISITION_H_
#define APPLICATIONS_ACQUISITION_H_

int acquisition_init(void);

/* Zephyr thread entry. */
void acquisition_thread(void *p1, void *p2, void *p3);

#endif

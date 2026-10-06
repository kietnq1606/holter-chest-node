#ifndef APPLICATIONS_ACQUISITION_H_
#define APPLICATIONS_ACQUISITION_H_

/* Initialize before starting the thread. */
int acquisition_init(void);

/* Queue commands; only the acquisition thread changes sampling state. */
int acquisition_start(void);
/* Completion follows the final block handoff, not the storage commit. */
int acquisition_stop(void);

/* Zephyr thread entry. */
void acquisition_thread(void *p1, void *p2, void *p3);

#endif

#ifndef CALIBRATION_H_
#define CALIBRATION_H_

#include <stdint.h>

/* Exclusive ADC/mux access, thread context only. Nominal run time is 7 s.
 * Output: microvolts per count scaled by 65536, unchanged on error.
 * Uses a nominal 1000 uV reference; attempts to restore ECG after capture.
 */
int ecg_calibrate(uint32_t *coefficient);

#endif /* CALIBRATION_H_ */

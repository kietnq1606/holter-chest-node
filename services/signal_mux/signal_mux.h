#ifndef SIGNAL_MUX_H_
#define SIGNAL_MUX_H_

/* Call once before starting threads. Selects ECG on success. */
int signal_mux_init(void);

/* Thread context only. Returns 0 or negative errno; does not wait for settling.
 * The caller must serialize all calls and own calibration and ADC sampling.
 */
/* Mode 0: nominal 0 mV reference (SEL_CAL=0, SEL_PULSE=0). */
int signal_mux_mode0(void);
/* Mode 1: nominal +1 mV reference (SEL_CAL=0, SEL_PULSE=1). */
int signal_mux_mode1(void);
/* Mode 2: ECG electrodes (SEL_CAL=1, SEL_PULSE=0). */
int signal_mux_mode2(void);

#endif /* SIGNAL_MUX_H_ */

#ifndef AD8232_H
#define AD8232_H


#include "stdbool.h"
#include "stdint.h"
#include "zephyr/device.h"
#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif
/* Define function pointer */

// read raw data from the AD8232 device
typedef int (*ad8232_read_raw_t)(const struct device *dev, 
                                int16_t *value);

// read LO+ and LO- status from the AD8232 device
typedef int (*ad8232_lead_off_t)(const struct device *dev,
                                bool *lead_off);

//driver API
/* Raw samples remain valid until this buffer is successfully released. */
struct ad8232_buffer {
    const int16_t *samples;
    uint16_t count;
};

/* Start hardware-triggered ECG sampling at 500 Hz. */
typedef int (*ad8232_start_t)(const struct device *dev);
/* Stop sampling and wait until DMA no longer accesses the buffers. */
typedef int (*ad8232_stop_t)(const struct device *dev);
/* Wait up to timeout for a completed raw-sample buffer; transfer ownership on success. */
typedef int (*ad8232_buffer_get_t)(const struct device *dev,
                                   struct ad8232_buffer *buffer,
                                   k_timeout_t timeout);
/* Return an acquired buffer for DMA reuse; samples must not be accessed after success. */
typedef int (*ad8232_buffer_release_t)(const struct device *dev,
                                       const struct ad8232_buffer *buffer);

__subsystem struct ad8232_driver_api {
    ad8232_read_raw_t read_raw;
    ad8232_lead_off_t get_lo_plus;
    ad8232_lead_off_t get_lo_minus;
    ad8232_start_t start;
    ad8232_stop_t stop;
    ad8232_buffer_get_t buffer_get;
    ad8232_buffer_release_t buffer_release;
};

/*Public API wrapper*/

// get data readed from ADC 12bit of AD8232 
static inline int ad8232_read_raw(const struct device *dev, int16_t *value)
{
    // get info hw configed in device tree of ad8232
    const struct ad8232_driver_api *api = DEVICE_API_GET(ad8232, dev);
    return api->read_raw(dev, value);
}

// get LO+ status from the AD8232 device
static inline int ad8232_get_lo_plus(const struct device *dev, bool *lead_off)
{
    // get info hw configed in device tree of ad8232
    const struct ad8232_driver_api *api = DEVICE_API_GET(ad8232, dev);
    return api->get_lo_plus(dev, lead_off);
}

// get LO- status from the AD8232 device
static inline int ad8232_get_lo_minus(const struct device *dev, bool *lead_off)
{
    // get info hw configed in device tree of ad8232
    const struct ad8232_driver_api *api = DEVICE_API_GET(ad8232, dev);
    return api->get_lo_minus(dev, lead_off);
}
/* Start hardware sampling at 500 Hz; reject restart with outstanding buffers. */
static inline int ad8232_start(const struct device *dev)
{
    const struct ad8232_driver_api *api = DEVICE_API_GET(ad8232, dev);
    return api->start(dev);
}

/* Stop DMA access without revoking buffers already owned by the caller. */
static inline int ad8232_stop(const struct device *dev)
{
    const struct ad8232_driver_api *api = DEVICE_API_GET(ad8232, dev);
    return api->stop(dev);
}

/* On success, the caller owns one completed raw-sample buffer. */
static inline int ad8232_buffer_get(const struct device *dev,
                                    struct ad8232_buffer *buffer,
                                    k_timeout_t timeout)
{
    const struct ad8232_driver_api *api = DEVICE_API_GET(ad8232, dev);
    return api->buffer_get(dev, buffer, timeout);
}

/* Release exactly once; do not access samples after a successful release. */
static inline int ad8232_buffer_release(const struct device *dev,
                                        const struct ad8232_buffer *buffer)
{
    const struct ad8232_driver_api *api = DEVICE_API_GET(ad8232, dev);
    return api->buffer_release(dev, buffer);
}

#ifdef __cplusplus
}
#endif
#endif /* AD8232_H */


#

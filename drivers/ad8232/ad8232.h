#ifndef AD8232_H
#define AD8232_H


#include "stdbool.h"
#include "stdint.h"
#include "zephyr/device.h"

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
__subsystem struct ad8232_driver_api {
    ad8232_read_raw_t read_raw;
    ad8232_lead_off_t get_lo_plus;
    ad8232_lead_off_t get_lo_minus;
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
#ifdef __cplusplus
}
#endif
#endif /* AD8232_H */


#

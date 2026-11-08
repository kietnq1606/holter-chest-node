#ifndef BMI323_I2C_H
#define BMI323_I2C_H

#include "stdbool.h"
#include "stdint.h"
#include "zephyr/device.h"


#ifdef __cplusplus
extern "C" {
#endif

struct bmi323_sample {
    int16_t accel_x;
    int16_t accel_y;
    int16_t accel_z;

    int16_t gyro_x;
    int16_t gyro_y;
    int16_t gyro_z;
};

// callback function type for when a new sample is ready
typedef void (*bmi323_callback_t)(const struct device *dev);
// read a sample 
typedef int (*bmi323_read_raw_t)(const struct device *dev, struct bmi323_sample *sample);
// set the callback function
typedef int (*bmi323_set_data_ready_callback_t)(const struct device *dev, bmi323_callback_t callback);

__subsystem struct bmi323_driver_api {
      bmi323_read_raw_t read_raw_sample;
      bmi323_set_data_ready_callback_t set_data_ready_callback;
};

static inline int bmi323_read_raw_sample(const struct device *dev, struct bmi323_sample *sample)
{
      return DEVICE_API_GET(bmi323, dev)->read_raw_sample(dev, sample);
}   

static inline int bmi323_set_data_ready_callback(const struct device *dev, bmi323_callback_t callback)
{
      return DEVICE_API_GET(bmi323, dev)->set_data_ready_callback(dev, callback);
}



#ifdef __cplusplus
}
#endif


#endif // BMI323_I2C_H

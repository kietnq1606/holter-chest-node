#define DT_DRV_COMPAT adi_ad8232

#include "ad8232.h"
#include <errno.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/irq.h>
#include <nrfx.h>
#include <nrfx_saadc.h>
#include <nrfx_timer.h>
#include <helpers/nrfx_gppi.h>

#define AD8232_SAMPLE_RATE     500U
#define AD8232_TIMER_HZ        1000000U
#define AD8232_BUFFER_SAMPLES  50U
#define AD8232_BUFFER_COUNT    2U
#define AD8232_STOP_TIMEOUT_US 1000U
/* Resolve io-channels = <&adc N> to the controller and its channel@N settings. */
#define AD8232_ADC_NODE(inst)  DT_IO_CHANNELS_CTLR(DT_DRV_INST(inst))
#define AD8232_CHANNEL(inst) \
    DT_CHILD(AD8232_ADC_NODE(inst), \
             UTIL_CAT(channel_, DT_IO_CHANNELS_INPUT(DT_DRV_INST(inst))))

/* The nRF52840 has one SAADC and nrfx has one SAADC callback context. */
BUILD_ASSERT(DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) == 1);
BUILD_ASSERT(!IS_ENABLED(CONFIG_ADC_NRFX_SAADC),
             "AD8232 streaming must exclusively own SAADC");

enum buffer_owner {
    BUFFER_FREE,   /* Available; neither DMA nor the application owns it. */
    BUFFER_DMA,    /* Registered as the current or next DMA destination. */
    BUFFER_READY,  /* Filled and queued, but not yet acquired by the caller. */
    BUFFER_CALLER, /* Acquired; DMA cannot reuse it until release succeeds. */
};

/* Queue a DMA buffer index; BUFFER_COUNT is reserved for a wake/error event. */
struct ad8232_ready_event {
    uint8_t index;
};

/* Immutable hardware settings from devicetree, accessed through dev->config. */
struct ad8232_config {
    struct gpio_dt_spec lo_plus;
    struct gpio_dt_spec lo_minus;
    nrfx_analog_input_t input;
    uint8_t channel_id;
};

/* Mutable state for this device instance, accessed through dev->data. */
struct ad8232_data {
    nrfx_timer_t timer;
    struct k_mutex control;   /* Serialize thread-side lifecycle and ownership changes. */
    struct k_msgq ready_queue; /* Transfer buffer indices from ISR to thread. */
    /* Queue backing memory holds notifications, not ECG samples. */
    char ready_storage[AD8232_BUFFER_COUNT * sizeof(struct ad8232_ready_event)]
        __aligned(4);
    /* 2 * 50 * 2 = 200 bytes; each buffer represents 100 ms at 500 Hz. */
    nrf_saadc_value_t samples[AD8232_BUFFER_COUNT][AD8232_BUFFER_SAMPLES]
        __aligned(4);
    enum buffer_owner owner[AD8232_BUFFER_COUNT];
    nrfx_gppi_handle_t sample_link;  /* TIMER COMPARE -> SAADC SAMPLE. */
    nrfx_gppi_handle_t restart_link; /* SAADC END -> SAADC START. */
    int error;              /* Latched fault returned by buffer_get(). */
    bool running;           /* Sampling triggers and callbacks are active. */
    bool saadc_initialized; /* Cleanup may still be needed after sampling stops. */
};

static void ad8232_saadc_callback(const nrfx_saadc_evt_t *event);

/* Call with interrupts locked; errors are returned/logged in thread context. */
static void stream_fault(struct ad8232_data *data, int err)
{
    /* Stop TIMER counting so it no longer generates periodic SAMPLE triggers. */
    nrfx_timer_disable(&data->timer);
    /* Disconnect TIMER COMPARE -> SAADC SAMPLE to block new ADC samples. */
    nrfx_gppi_conn_disable(data->sample_link);
    /* Disconnect SAADC END -> START to prevent automatic DMA-buffer restart.
     * These calls do not wait for an in-flight conversion; stop_locked() does.
     */
    nrfx_gppi_conn_disable(data->restart_link);
    data->running = false;
    data->error = err;

    /* Wake a blocked getter. If full, it already has completed buffers to read. */
    struct ad8232_ready_event wake = {
        .index = AD8232_BUFFER_COUNT,
    };
    (void)k_msgq_put(&data->ready_queue, &wake, K_NO_WAIT);
}

static void ad8232_saadc_callback(const nrfx_saadc_evt_t *event)
{
    /* nrfx has no device argument; the single-instance assertion permits index 0. */
    const struct device *dev = DEVICE_DT_INST_GET(0);
    struct ad8232_data *data = dev->data;

    if (!data->running) {
        /* Ignore late/partial conversion callbacks during shutdown. */
        return;
    }

    switch (event->type) {
    case NRFX_SAADC_EVT_READY:
        /* The first DMA buffer is latched before the first 2 ms trigger. */
        nrfx_timer_enable(&data->timer);
        break;

    case NRFX_SAADC_EVT_BUF_REQ:
        /* No replacement is registered yet. Prevent END from restarting DMA
         * with an old pointer; release() rearms the link after supplying one.
         */
        nrfx_gppi_conn_disable(data->restart_link);
        break;

    case NRFX_SAADC_EVT_DONE: {
        uint8_t index;

        /* Identify which of our two buffers DMA has finished writing. */
        for (index = 0; index < AD8232_BUFFER_COUNT; index++) {
            if (event->data.done.p_buffer == data->samples[index]) {
                break;
            }
        }
        /* Accept only complete 50-sample blocks previously assigned to DMA. */
        if (index == AD8232_BUFFER_COUNT ||
            event->data.done.size != AD8232_BUFFER_SAMPLES ||
            data->owner[index] != BUFFER_DMA) {
            stream_fault(data, -EIO);
            return;
        }

        struct ad8232_ready_event ready = {
            .index = index,
        };
        /* Queue only the index; raw data stays in its original DMA buffer. */
        data->owner[index] = BUFFER_READY;
        if (k_msgq_put(&data->ready_queue, &ready, K_NO_WAIT) != 0) {
            stream_fault(data, -ENOBUFS);
        }
        break;
    }

    case NRFX_SAADC_EVT_FINISHED:
        /* Both buffers were consumed before the caller supplied a replacement. */
        stream_fault(data, -ENOBUFS);
        break;

    default:
        break;
    }
}

/* Only this driver owns SAADC; GPIO lead-off access remains independent. */
static int prepare_saadc(const struct device *dev)
{
    const struct ad8232_config *config = dev->config;
    struct ad8232_data *data = dev->data;
    nrfx_saadc_channel_t channel =
        NRFX_SAADC_DEFAULT_CHANNEL_SE(config->input, config->channel_id);

    int err = nrfx_saadc_init(DT_IRQ(AD8232_ADC_NODE(0), priority));
    if (err != 0) {
        /* Do not uninitialize SAADC if another owner made init fail. */
        return err;
    }
    data->saadc_initialized = true;

    /* Match the calibrated DT path: internal 0.6 V, gain 1/6, acquisition 10 us.
     * 12-bit raw scale: 0.6 / (1/6) / 4096 = 878.90625 uV/count at ADC input.
     * The driver returns raw counts; baseline and ECG gain stay in the application.
     */
    channel.channel_config.gain = NRF_SAADC_GAIN1_6;
    channel.channel_config.reference = NRF_SAADC_REFERENCE_INTERNAL;
    channel.channel_config.acq_time = NRF_SAADC_ACQTIME_10US;
    err = nrfx_saadc_channels_config(&channel, 1);
    if (err == 0) {
        /* SAADC offset calibration is not AD8232 analog gain calibration. */
        err = nrfx_saadc_offset_calibrate(NULL);
    }
    if (err != 0) {
        /* Roll back only the SAADC instance initialized by this call. */
        nrfx_saadc_uninit();
        data->saadc_initialized = false;
    }
    return err;
}

/* Serialize lifecycle calls with control; protect DMA ownership with irq_lock. */
static int stop_locked(struct ad8232_data *data)
{
    /* Keep the ISR from changing ownership while shutdown updates state. */
    unsigned int key = irq_lock();

    /* Stop TIMER counting so it no longer generates periodic SAMPLE triggers. */
    nrfx_timer_disable(&data->timer);
    /* Disconnect TIMER COMPARE -> SAADC SAMPLE to block new ADC samples. */
    nrfx_gppi_conn_disable(data->sample_link);
    /* Disconnect SAADC END -> START to prevent automatic DMA-buffer restart.
     * These calls do not wait for an in-flight conversion; stop_locked() does.
     */
    nrfx_gppi_conn_disable(data->restart_link);
    data->running = false;

    if (data->saadc_initialized) {
        /* Disable trigger paths before STOP. END alone can refer to an older
         * buffer; wait specifically for this STOPPED before recycling RAM.
         */
        if (nrf_saadc_enable_check(NRF_SAADC)) {
            nrf_saadc_event_clear(NRF_SAADC, NRF_SAADC_EVENT_STOPPED);
            nrf_saadc_task_trigger(NRF_SAADC, NRF_SAADC_TASK_STOP);
            if (!WAIT_FOR(nrf_saadc_event_check(NRF_SAADC, NRF_SAADC_EVENT_STOPPED),
                          AD8232_STOP_TIMEOUT_US, k_busy_wait(1))) {
                /* DMA safety is unconfirmed: keep buffers reserved on timeout. */
                stream_fault(data, -ETIMEDOUT);
                irq_unlock(key);
                return -ETIMEDOUT;
            }
        }
        nrfx_saadc_uninit();
        data->saadc_initialized = false;
    }

    /* Stop discards queued/partial data, but never revokes caller-owned buffers. */
    data->error = -ECANCELED;
    k_msgq_purge(&data->ready_queue);
    for (size_t i = 0; i < AD8232_BUFFER_COUNT; i++) {
        if (data->owner[i] != BUFFER_CALLER) {
            data->owner[i] = BUFFER_FREE;
        }
    }
    struct ad8232_ready_event wake = {
        .index = AD8232_BUFFER_COUNT,
    };
    /* Let a blocked getter observe cancellation instead of waiting forever. */
    (void)k_msgq_put(&data->ready_queue, &wake, K_NO_WAIT);
    irq_unlock(key);
    return 0;
}

static int ad8232_start_impl(const struct device *dev)
{
    const struct ad8232_config *config = dev->config;
    struct ad8232_data *data = dev->data;
    int err;

    /* Initialization/calibration and mutex waits require thread context. */
    if (k_is_in_isr()) {
        return -EWOULDBLOCK;
    }
    k_mutex_lock(&data->control, K_FOREVER);
    if (data->running) {
        /* Do not reset an active stream or its sample cadence. */
        err = -EALREADY;
        goto out;
    }
    for (size_t i = 0; i < AD8232_BUFFER_COUNT; i++) {
        if (data->owner[i] == BUFFER_CALLER) {
            /* Restart would otherwise overwrite samples still in use. */
            err = -EBUSY;
            goto out;
        }
    }
    /* Clean up an earlier fault before reconfiguring the peripheral. */
    err = stop_locked(data);
    if (err != 0) {
        goto out;
    }
    err = prepare_saadc(dev);
    if (err != 0) {
        goto out;
    }

    nrfx_saadc_adv_config_t advanced = NRFX_SAADC_DEFAULT_ADV_CONFIG;
    /* SAMPLE comes from TIMER/PPI; END->START is also a hardware PPI link. */
    advanced.internal_timer_cc = 0;
    advanced.start_on_end = false;
    err = nrfx_saadc_advanced_mode_set(BIT(config->channel_id),
                                      NRF_SAADC_RESOLUTION_12BIT,
                                      &advanced, ad8232_saadc_callback);
    if (err != 0) {
        (void)stop_locked(data);
        goto out;
    }

    unsigned int key = irq_lock();

    /* Publish the new stream state before the SAADC ISR can observe it. */
    data->error = 0;
    k_msgq_purge(&data->ready_queue);
    nrfx_timer_clear(&data->timer);
    /* Prime both DMA destinations before enabling periodic sample triggers. */
    for (size_t i = 0; i < AD8232_BUFFER_COUNT; i++) {
        err = nrfx_saadc_buffer_set(data->samples[i], AD8232_BUFFER_SAMPLES);
        if (err != 0) {
            break;
        }
        data->owner[i] = BUFFER_DMA;
    }
    if (err == 0) {
        data->running = true;
        nrfx_gppi_conn_enable(data->sample_link);
        nrfx_gppi_conn_enable(data->restart_link);
        /* Start latching DMA; READY starts TIMER only after the first buffer is ready. */
        err = nrfx_saadc_mode_trigger();
    }
    if (err != 0) {
        (void)stop_locked(data);
    }
    irq_unlock(key);
out:
    k_mutex_unlock(&data->control);
    return err;
}

static int ad8232_stop_impl(const struct device *dev)
{
    struct ad8232_data *data = dev->data;

    if (k_is_in_isr()) {
        return -EWOULDBLOCK;
    }
    k_mutex_lock(&data->control, K_FOREVER);
    int err = stop_locked(data);
    k_mutex_unlock(&data->control);
    return err;
}

static int ad8232_buffer_get_impl(const struct device *dev,
                                  struct ad8232_buffer *buffer,
                                  k_timeout_t timeout)
{
    struct ad8232_data *data = dev->data;
    struct ad8232_ready_event event;

    if (buffer == NULL) {
        return -EINVAL;
    }
    if (k_is_in_isr()) {
        return -EWOULDBLOCK;
    }

    /* Drain completed buffers before returning a latched stream error. */
    int err = k_msgq_get(&data->ready_queue, &event, K_NO_WAIT);
    if (err != 0) {
        unsigned int key = irq_lock();
        err = data->error;
        irq_unlock(key);
        if (err != 0) {
            return err;
        }
        /* Wait without holding control, so other lifecycle operations can finish. */
        err = k_msgq_get(&data->ready_queue, &event, timeout);
        if (err != 0) {
            return err;
        }
    }

    /* The caller must finish pending get calls before stopping or restarting.
     * The mutex protects ownership transfer, not the preceding queue wait.
     */
    k_mutex_lock(&data->control, K_FOREVER);
    unsigned int key = irq_lock();
    if (event.index >= AD8232_BUFFER_COUNT) {
        /* The reserved index is a notification, never an array subscript. */
        err = data->error != 0 ? data->error : -ECANCELED;
    } else if (data->owner[event.index] != BUFFER_READY) {
        err = -EIO;
    } else {
        /* Return the original buffer without copying its 100-byte payload. */
        data->owner[event.index] = BUFFER_CALLER;
        buffer->samples = data->samples[event.index];
        buffer->count = AD8232_BUFFER_SAMPLES;
        err = 0;
    }
    irq_unlock(key);
    k_mutex_unlock(&data->control);
    return err;
}

static int ad8232_buffer_release_impl(const struct device *dev,
                                      const struct ad8232_buffer *buffer)
{
    struct ad8232_data *data = dev->data;
    uint8_t index;
    int err = 0;

    if (buffer == NULL || buffer->count != AD8232_BUFFER_SAMPLES) {
        return -EINVAL;
    }
    if (k_is_in_isr()) {
        return -EWOULDBLOCK;
    }
    k_mutex_lock(&data->control, K_FOREVER);
    unsigned int key = irq_lock();

    /* Match exact buffer addresses; interior or foreign pointers are invalid. */
    for (index = 0; index < AD8232_BUFFER_COUNT; index++) {
        if (buffer->samples == data->samples[index]) {
            break;
        }
    }
    if (index == AD8232_BUFFER_COUNT || data->owner[index] != BUFFER_CALLER) {
        /* Reject invalid pointers and buffers already released. */
        err = -EINVAL;
    } else if (!data->running) {
        /* After stop/fault, release ownership without scheduling more DMA. */
        data->owner[index] = BUFFER_FREE;
    } else if (nrf_saadc_event_check(NRF_SAADC, NRF_SAADC_EVENT_END)) {
        /* Re-enabling PPI cannot replay an END that already happened. */
        stream_fault(data, -EOVERFLOW);
        err = -EOVERFLOW;
    } else {
        /* Protect nrfx state from the ISR; never reuse a buffer before release. */
        err = nrfx_saadc_buffer_set(data->samples[index], AD8232_BUFFER_SAMPLES);
        if (err == 0) {
            data->owner[index] = BUFFER_DMA;
            nrfx_gppi_conn_enable(data->restart_link);
            /* Hardware can reach END even while CPU interrupts are locked.
             * Close the registration race; ownership has already transferred,
             * so report the stream fault through get(), not a failed release.
             */
            if (nrf_saadc_event_check(NRF_SAADC, NRF_SAADC_EVENT_END)) {
                stream_fault(data, -EOVERFLOW);
            }
        }
    }
    irq_unlock(key);
    k_mutex_unlock(&data->control);
    return err;
}

static int ad8232_read_raw_impl(const struct device *dev, int16_t *value)
{
    const struct ad8232_config *config = dev->config;
    struct ad8232_data *data = dev->data;
    int err;

    if (value == NULL) {
        return -EINVAL;
    }
    if (k_is_in_isr()) {
        return -EWOULDBLOCK;
    }
    k_mutex_lock(&data->control, K_FOREVER);
    /* Single reads must not reconfigure a live stream or overwrite held data. */
    if (data->saadc_initialized ||
        data->owner[0] != BUFFER_FREE || data->owner[1] != BUFFER_FREE) {
        err = -EBUSY;
        goto out;
    }
    err = prepare_saadc(dev);
    if (err != 0) {
        goto out;
    }
    err = nrfx_saadc_simple_mode_set(BIT(config->channel_id),
                                    NRF_SAADC_RESOLUTION_12BIT,
                                    NRF_SAADC_OVERSAMPLE_DISABLED, NULL);
    if (err == 0) {
        /* Reuse idle DMA memory for one sample; no extra raw buffer is needed. */
        err = nrfx_saadc_buffer_set(data->samples[0], 1);
    }
    if (err == 0) {
        /* A NULL simple-mode callback makes this conversion blocking. */
        err = nrfx_saadc_mode_trigger();
    }
    if (err == 0) {
        /* Return status separately from the raw sample; never subtract baseline here. */
        *value = data->samples[0][0];
    }
    nrfx_saadc_uninit();
    data->saadc_initialized = false;
out:
    k_mutex_unlock(&data->control);
    return err;
}

static int ad8232_get_lo_plus_impl(const struct device *dev, bool *lead_off)
{
    const struct ad8232_config *config = dev->config;

    if (lead_off == NULL) {
        return -EINVAL;
    }
    int err = gpio_pin_get_dt(&config->lo_plus);
    if (err < 0) {
        return err;
    }
    /* gpio_pin_get_dt() applies the polarity declared in devicetree. */
    *lead_off = (err != 0);
    return 0;
}

static int ad8232_get_lo_minus_impl(const struct device *dev, bool *lead_off)
{
    const struct ad8232_config *config = dev->config;

    if (lead_off == NULL) {
        return -EINVAL;
    }
    int err = gpio_pin_get_dt(&config->lo_minus);
    if (err < 0) {
        return err;
    }
    /* Negative values are errors, not an asserted lead-off indication. */
    *lead_off = (err != 0);
    return 0;
}

static int ad8232_init(const struct device *dev)
{
    const struct ad8232_config *config = dev->config;
    struct ad8232_data *data = dev->data;
    int err;

    /* Route SAADC interrupts to nrfx; callbacks later notify the acquisition thread. */
    IRQ_CONNECT(DT_IRQN(AD8232_ADC_NODE(0)),
                DT_IRQ(AD8232_ADC_NODE(0), priority),
                nrfx_isr, nrfx_saadc_irq_handler, 0);

    if (!gpio_is_ready_dt(&config->lo_plus) ||
        !gpio_is_ready_dt(&config->lo_minus)) {
        return -ENODEV;
    }
    err = gpio_pin_configure_dt(&config->lo_plus, GPIO_INPUT);
    if (err != 0) {
        return err;
    }
    err = gpio_pin_configure_dt(&config->lo_minus, GPIO_INPUT);
    if (err != 0) {
        return err;
    }
    k_mutex_init(&data->control);
    k_msgq_init(&data->ready_queue, data->ready_storage,
                sizeof(struct ad8232_ready_event), AD8232_BUFFER_COUNT);
    /* Before start(), getters must report stopped rather than wait for data. */
    data->error = -ECANCELED;

    nrfx_timer_config_t timer_config = NRFX_TIMER_DEFAULT_CONFIG(AD8232_TIMER_HZ);
    err = nrfx_timer_init(&data->timer, &timer_config, NULL);
    if (err != 0) {
        return err;
    }
    /* ticks = 1,000,000 / 500 = 2000: exactly 2 ms per hardware trigger. */
    uint32_t ticks = AD8232_TIMER_HZ / AD8232_SAMPLE_RATE;
    nrfx_timer_extended_compare(&data->timer, NRF_TIMER_CC_CHANNEL0, ticks,
                                NRF_TIMER_SHORT_COMPARE0_CLEAR_MASK, false);

    /* COMPARE0_CLEAR repeats the period in hardware; TIMER interrupts are disabled. */
    uint32_t timer_event = nrfx_timer_compare_event_address_get(
        &data->timer, NRF_TIMER_CC_CHANNEL0);
    uint32_t sample_task = nrf_saadc_task_address_get(NRF_SAADC, NRF_SAADC_TASK_SAMPLE);
    uint32_t end_event = nrf_saadc_event_address_get(NRF_SAADC, NRF_SAADC_EVENT_END);
    uint32_t start_task = nrf_saadc_task_address_get(NRF_SAADC, NRF_SAADC_TASK_START);

    /* Allocate links now but enable them only when starting a stream. */
    err = nrfx_gppi_conn_alloc(timer_event, sample_task, &data->sample_link);
    if (err != 0) {
        nrfx_timer_uninit(&data->timer);
        return err;
    }
    err = nrfx_gppi_conn_alloc(end_event, start_task, &data->restart_link);
    if (err != 0) {
        /* Release the first link if allocating the second one fails. */
        nrfx_gppi_conn_free(timer_event, sample_task, data->sample_link);
        nrfx_timer_uninit(&data->timer);
        return err;
    }
    return 0;
}

/* Public header wrappers dispatch through this device-specific function table. */
static DEVICE_API(ad8232, ad8232_api) = {
    .read_raw = ad8232_read_raw_impl,
    .get_lo_plus = ad8232_get_lo_plus_impl,
    .get_lo_minus = ad8232_get_lo_minus_impl,
    .start = ad8232_start_impl,
    .stop = ad8232_stop_impl,
    .buffer_get = ad8232_buffer_get_impl,
    .buffer_release = ad8232_buffer_release_impl,
};

/* Validate the DT settings rather than silently changing calibration scale.
 * The reserved timer is owned by this driver, not a Zephyr counter driver.
 * DEVICE_DT_INST_DEFINE binds immutable config, mutable data and the API table.
 */
#define DRIVER_DEFINE(inst) \
    BUILD_ASSERT(DT_SAME_NODE(AD8232_ADC_NODE(inst), DT_NODELABEL(adc))); \
    BUILD_ASSERT(DT_STRING_TOKEN(AD8232_CHANNEL(inst), zephyr_gain) == ADC_GAIN_1_6); \
    BUILD_ASSERT(DT_STRING_TOKEN(AD8232_CHANNEL(inst), zephyr_reference) == ADC_REF_INTERNAL); \
    BUILD_ASSERT(DT_PROP(AD8232_CHANNEL(inst), zephyr_resolution) == 12); \
    BUILD_ASSERT(!DT_PROP_OR(AD8232_CHANNEL(inst), zephyr_differential, 0)); \
    BUILD_ASSERT(DT_PROP_OR(AD8232_CHANNEL(inst), zephyr_oversampling, 0) == 0); \
    BUILD_ASSERT(DT_PROP(AD8232_CHANNEL(inst), zephyr_acquisition_time) == ADC_ACQ_TIME_DEFAULT || \
                 DT_PROP(AD8232_CHANNEL(inst), zephyr_acquisition_time) == \
                 ADC_ACQ_TIME(ADC_ACQ_TIME_MICROSECONDS, 10)); \
    BUILD_ASSERT(DT_PROP(AD8232_CHANNEL(inst), zephyr_input_positive) < 8); \
    BUILD_ASSERT(DT_IO_CHANNELS_INPUT(DT_DRV_INST(inst)) < 8); \
    BUILD_ASSERT(DT_NODE_HAS_STATUS(DT_PHANDLE(DT_DRV_INST(inst), sample_timer), reserved)); \
    static struct ad8232_data ad8232_data_##inst = { \
        .timer = NRFX_TIMER_INSTANCE(DT_REG_ADDR( \
            DT_PHANDLE(DT_DRV_INST(inst), sample_timer))), \
    }; \
    static const struct ad8232_config ad8232_config_##inst = { \
        .lo_plus = GPIO_DT_SPEC_INST_GET(inst, lo_plus_gpios), \
        .lo_minus = GPIO_DT_SPEC_INST_GET(inst, lo_minus_gpios), \
        .input = (nrfx_analog_input_t)DT_PROP(AD8232_CHANNEL(inst), zephyr_input_positive), \
        .channel_id = DT_IO_CHANNELS_INPUT(DT_DRV_INST(inst)), \
    }; \
    DEVICE_DT_INST_DEFINE(inst, ad8232_init, NULL, &ad8232_data_##inst, \
                          &ad8232_config_##inst, POST_KERNEL, 51, &ad8232_api);

DT_INST_FOREACH_STATUS_OKAY(DRIVER_DEFINE)

#include "acq_thread.h"
#include "ad8232.h"
#include "bmi323_i2c.h"
#include "storage.h"
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>

/* Packet: header (1 B), 50 ECG (100 B), AX/AY/AZ/GX/GY/GZ (12 B), tail (1 B).
 * Samples are signed 16-bit little-endian. Tags are checked at fixed positions.
 */
#define ACQ_PACKET_HEADER       0x00U
#define ACQ_PACKET_TAIL         0x99U
#define ACQ_ECG_SAMPLES         50U
#define ACQ_BMI_AXES            6U
#define ACQ_PACKET_SIZE         114U

/* Nominal 1.65 V baseline; verify against the actual board. */
#define ECG_BASELINE_ADC 1877

static const struct device *const ad8232_dev = DEVICE_DT_GET(DT_NODELABEL(ad8232));
static const struct device *const bmi323_dev = DEVICE_DT_GET(DT_NODELABEL(bmi323));

enum acquisition_state {
    ACQ_IDLE = 0,
    ACQ_WAIT_BUFFER,
    ACQ_WAIT_SAMPLE,
    ACQ_READ_SAMPLE,
    ACQ_SUBMIT,
    ACQ_DONE,
};

static struct {
    uint8_t current_state;
    bool has_buffer;
    bool has_sample;   /* Acquisition owns a completed ECG DMA buffer. */
    bool buffer_ready; /* The 2046-byte storage buffer is full. */
    bool done;         /* Queued for storage, not necessarily committed to Flash. */
} acq_app_flags = {
    .current_state = ACQ_IDLE,
};

static struct {
    struct k_msgq *free_queue;  /* Buffer pointers returned by storage. */
    struct k_msgq *ready_queue; /* Full buffer pointers handed to storage. */
    uint8_t *active_buffer;    /* Writable only while acquisition owns it. */
    size_t write_pos;          /* Next byte in the active storage buffer. */
    struct ad8232_buffer ecg_buffer; /* Keep until release succeeds. */
    /* Keep a complete packet while it crosses a storage-buffer boundary. */
    uint8_t packet[ACQ_PACKET_SIZE];
    uint8_t packet_remaining; /* Completed packet bytes still awaiting a copy. */
    bool ecg_copied;     /* Packet already holds ECG if a release needs retrying. */
    bool stream_running; /* Sampling is paused when no storage buffer is available. */
} acq_resources;

static void copy_pending_packet(void);
static void read_bmi_sample(void);
static int release_ecg_buffer(void);
static int stop_ecg_stream(void);
static void fsm_change_state(void);
static void fsm_handle_state(void);
static void idle_handler(void);
static void wait_buffer_handler(void);
static void wait_sample_handler(void);
static void read_sample_handler(void);
static void submit_handler(void);
static void done_handler(void);

int acquisition_init(void)
{
    if (!device_is_ready(ad8232_dev)) {
        return -ENODEV;
    }

    /* BMI is optional: an unavailable IMU must not prevent ECG acquisition. */
    if (!device_is_ready(bmi323_dev)) {
        printk("Acquisition: BMI unavailable; ECG remains enabled\n");
    } else {
        /* Poll one latest sample per ECG block; no data-ready flag is needed. */
        int err = bmi323_set_data_ready_callback(bmi323_dev, NULL);
        if (err != 0) {
            printk("Acquisition: BMI callback reset failed (%d)\n", err);
        }
    }
    return 0;
}

void acquisition_thread(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p3);
    acq_resources.free_queue = p1;
    acq_resources.ready_queue = p2;

    if (p1 == NULL || p2 == NULL) {
        printk("Acquisition: invalid queues\n");
        return;
    }

    /* Handlers update flags before the FSM evaluates transitions. */
    while (1) {
        fsm_handle_state();
        fsm_change_state();
    }
}

/* DONE completes one buffer handoff; acquisition then requests another buffer. */
static void fsm_change_state(void)
{
    switch (acq_app_flags.current_state) {
    case ACQ_IDLE:
        acq_app_flags.current_state = ACQ_WAIT_BUFFER;
        break;

    case ACQ_WAIT_BUFFER:
        if (acq_app_flags.has_buffer) {
            acq_app_flags.current_state = ACQ_WAIT_SAMPLE;
        }
        break;

    case ACQ_WAIT_SAMPLE:
        if (acq_app_flags.has_sample) {
            acq_app_flags.current_state = ACQ_READ_SAMPLE;
        }
        break;

    case ACQ_READ_SAMPLE:
        acq_app_flags.current_state = acq_app_flags.buffer_ready ? ACQ_SUBMIT : ACQ_WAIT_SAMPLE;
        break;

    case ACQ_SUBMIT:
        if (acq_app_flags.done) {
            acq_app_flags.current_state = ACQ_DONE;
        }
        break;

    case ACQ_DONE:
        acq_app_flags.current_state = ACQ_IDLE;
        break;

    default:
        printk("Acquisition: invalid state %u\n",
               (unsigned int)acq_app_flags.current_state);
        break;
    }
}
/* Dispatch one handler; state changes are kept in fsm_change_state(). */
static void fsm_handle_state(void)
{
    switch (acq_app_flags.current_state) {
    case ACQ_IDLE:
        idle_handler();
        break;

    case ACQ_WAIT_BUFFER:
        wait_buffer_handler();
        break;

    case ACQ_WAIT_SAMPLE:
        wait_sample_handler();
        break;

    case ACQ_READ_SAMPLE:
        read_sample_handler();
        break;

    case ACQ_SUBMIT:
        submit_handler();
        break;

    case ACQ_DONE:
        done_handler();
        break;

    default:
        printk("Acquisition: invalid state %u\n",
               (unsigned int)acq_app_flags.current_state);
        break;
    }
}

/* Never discard valid ECG because the latest BMI sample is unavailable. */
static void read_bmi_sample(void)
{
    struct bmi323_sample bmi;
    uint8_t *destination = acq_resources.packet +
                           1U + ACQ_ECG_SAMPLES * sizeof(int16_t);
    int err = device_is_ready(bmi323_dev)
            ? bmi323_read_raw_sample(bmi323_dev, &bmi) : -ENODEV;

    if (err != 0) {
        /* Preserve the six-axis slots so packet framing remains unchanged. */
        for (size_t i = 0; i < ACQ_BMI_AXES; i++) {
            sys_put_le16((uint16_t)INT16_MIN, destination + i * sizeof(int16_t));
        }
        printk("Acquisition: BMI read failed (%d)\n", err);
        return;
    }
    sys_put_le16((uint16_t)bmi.accel_x, destination);
    sys_put_le16((uint16_t)bmi.accel_y, destination + 2);
    sys_put_le16((uint16_t)bmi.accel_z, destination + 4);
    sys_put_le16((uint16_t)bmi.gyro_x, destination + 6);
    sys_put_le16((uint16_t)bmi.gyro_y, destination + 8);
    sys_put_le16((uint16_t)bmi.gyro_z, destination + 10);
}

static int stop_ecg_stream(void)
{
    int err = ad8232_stop(ad8232_dev);

    /* The driver disables triggers even when waiting for STOPPED times out. */
    acq_resources.stream_running = false;
    return err;
}

static int release_ecg_buffer(void)
{
    int err = ad8232_buffer_release(ad8232_dev, &acq_resources.ecg_buffer);

    if (err == 0) {
        /* A successful release transfers ownership; never read this pointer again. */
        acq_resources.ecg_buffer.samples = NULL;
        acq_resources.ecg_buffer.count = 0U;
    }
    return err;
}

/* Copy only what fits; retain the remainder for the next buffer, without padding. */
static void copy_pending_packet(void)
{
    /* No completed packet bytes are waiting to be copied. */
    if (acq_resources.packet_remaining == 0U) {
        return;
    }

    /* Free bytes = buffer capacity (2046) - bytes already written. */
    size_t available = STORAGE_BLOCK_DATA_SIZE - acq_resources.write_pos;
    size_t length = acq_resources.packet_remaining;

    /* Copy length = min(packet bytes remaining, free buffer bytes). */
    if (length > available) {
        length = available;
    }
    /* Source offset = packet size (114) - bytes not yet copied.
     * Example: 20 bytes remaining means copying resumes at byte 94.
     */
    size_t offset = ACQ_PACKET_SIZE - acq_resources.packet_remaining;

    /* Append at write_pos, starting from the first uncopied packet byte. */
    memcpy(acq_resources.active_buffer + acq_resources.write_pos,
           acq_resources.packet + offset, length);
    /* Advance the destination and subtract the bytes copied this time. */
    acq_resources.write_pos += length;
    acq_resources.packet_remaining -= length;

    /* A full buffer goes to storage; any packet remainder stays for the next one. */
    acq_app_flags.buffer_ready = (acq_resources.write_pos == STORAGE_BLOCK_DATA_SIZE);
}

static void idle_handler(void){
}

static void wait_buffer_handler(void)
{
    acq_app_flags.has_buffer = false;

    if (acq_resources.active_buffer == NULL) {
        /* Immediate handoffs keep hardware sampling running without a restart. */
        int err = k_msgq_get(acq_resources.free_queue,
                            &acq_resources.active_buffer, K_NO_WAIT);
        if (err != 0) {
            /* Without storage capacity, stop DMA before blocking for a buffer.
             * This intentionally creates a gap; packet remainder is retained.
             */
            if (acq_resources.stream_running) {
                err = stop_ecg_stream();
                if (err != 0) {
                    printk("Acquisition: ECG stop failed (%d)\n", err);
                    k_msleep(10);
                    return;
                }
            }
            err = k_msgq_get(acq_resources.free_queue,
                            &acq_resources.active_buffer, K_FOREVER);
            if (err != 0) {
                printk("Acquisition: get buffer failed (%d)\n", err);
                return;
            }
        }
        if (acq_resources.active_buffer == NULL) {
            printk("Acquisition: null buffer\n");
            return;
        }
        acq_resources.write_pos = 0U;
        acq_app_flags.buffer_ready = false;
        /* Finish a split packet before acquiring the next ECG block. */
        copy_pending_packet();
    }
    acq_app_flags.has_buffer = true;
}

static void wait_sample_handler(void)
{
    acq_app_flags.has_sample = false;

    /* A failed release must be retried before acquiring another buffer. */
    if (acq_resources.ecg_buffer.samples != NULL) {
        acq_app_flags.has_sample = true;
        return;
    }
    if (!acq_resources.stream_running) {
        int err = ad8232_start(ad8232_dev);
        if (err != 0) {
            printk("Acquisition: ECG start failed (%d)\n", err);
            k_msleep(10);
            return;
        }
        acq_resources.stream_running = true;
    }

    /* Hardware collects 50 samples at 500 Hz; the thread waits about 100 ms. */
    int err = ad8232_buffer_get(ad8232_dev, &acq_resources.ecg_buffer, K_FOREVER);
    if (err != 0) {
        printk("Acquisition: ECG DMA receive failed (%d)\n", err);
        int stop_err = stop_ecg_stream();
        if (stop_err != 0) {
            printk("Acquisition: ECG stop failed (%d)\n", stop_err);
        }
        k_msleep(10);
        return;
    }
    acq_resources.ecg_copied = false;
    acq_app_flags.has_sample = true;
}

static void read_sample_handler(void)
{
    acq_app_flags.has_sample = false;

    if (acq_resources.ecg_buffer.count != ACQ_ECG_SAMPLES) {
        printk("Acquisition: unexpected ECG sample count (%u)\n",
               (unsigned int)acq_resources.ecg_buffer.count);
        int err = stop_ecg_stream();
        if (err != 0) {
            printk("Acquisition: ECG stop failed (%d)\n", err);
            return;
        }
        err = release_ecg_buffer();
        if (err != 0) {
            printk("Acquisition: ECG release failed (%d)\n", err);
        }
        return;
    }

    if (!acq_resources.ecg_copied) {
        for (size_t i = 0; i < ACQ_ECG_SAMPLES; i++) {
            /* DMA holds raw ADC counts; subtract the baseline exactly once. */
            int16_t ecg = acq_resources.ecg_buffer.samples[i] - ECG_BASELINE_ADC;
            sys_put_le16((uint16_t)ecg,
                         acq_resources.packet + 1U + i * sizeof(int16_t));
        }
        acq_resources.ecg_copied = true;
    }

    /* Return DMA immediately after copying ECG, before BMI I2C or storage work. */
    int err = release_ecg_buffer();
    if (err != 0) {
        printk("Acquisition: ECG release failed (%d)\n", err);
        int stop_err = stop_ecg_stream();
        if (stop_err != 0) {
            printk("Acquisition: ECG stop failed (%d)\n", stop_err);
        }
        /* Keep the descriptor and copied packet for a safe release retry. */
        return;
    }

    acq_resources.packet[0] = ACQ_PACKET_HEADER;
    read_bmi_sample();
    acq_resources.packet[ACQ_PACKET_SIZE - 1U] = ACQ_PACKET_TAIL;
    acq_resources.packet_remaining = ACQ_PACKET_SIZE;
    acq_resources.ecg_copied = false;
    copy_pending_packet();
}

static void submit_handler(void)
{
    /* The queue copies a pointer, not the payload; stop writing after success. */
    int err = k_msgq_put(acq_resources.ready_queue,
                        &acq_resources.active_buffer, K_MSEC(1));

    acq_app_flags.done = (err == 0);
    if (err != 0) {
        printk("Acquisition: submit failed (%d)\n", err);
        return;
    }
}

static void done_handler(void)
{
    /* Storage owns the submitted buffer; retain any packet remainder. */
    acq_resources.active_buffer = NULL;
    acq_resources.write_pos = 0U;
    acq_app_flags.has_buffer = false;
    acq_app_flags.has_sample = false;
    acq_app_flags.buffer_ready = false;
    acq_app_flags.done = false;
}

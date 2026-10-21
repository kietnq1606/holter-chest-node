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
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>

/* Packet: header (1 B), 50 ECG (100 B), AX/AY/AZ/GX/GY/GZ (12 B), tail (1 B).
 * Samples are signed 16-bit little-endian. Tags are checked at fixed positions.
 */
#define ACQ_PACKET_HEADER       0x00U
#define ACQ_PACKET_TAIL         0x99U
#define ACQ_ECG_SAMPLES         50U
#define ACQ_BMI_AXES            6U
#define ACQ_BMI_AVERAGE_SAMPLES 5U
#define ACQ_PACKET_SIZE         114U

/* Nominal 1.65 V baseline; verify against the actual board. */
#define ECG_BASELINE_ADC 1877

BUILD_ASSERT(CONFIG_SYS_CLOCK_TICKS_PER_SEC % 500U == 0U,
             "The kernel tick must represent a 2 ms period");
BUILD_ASSERT(ACQ_PACKET_SIZE <= UINT8_MAX);
BUILD_ASSERT(STORAGE_BLOCK_DATA_SIZE >= ACQ_PACKET_SIZE);

static const struct device *const ad8232_dev = DEVICE_DT_GET(DT_NODELABEL(ad8232));
static const struct device *const bmi323_dev = DEVICE_DT_GET(DT_NODELABEL(bmi323));

/* The thread waits for timer expiry; ADC and I2C reads stay outside the ISR. */
K_TIMER_DEFINE(ecg_timer, NULL, NULL);
static atomic_t bmi_ready;

static void bmi_data_ready_callback(const struct device *dev)
{
    ARG_UNUSED(dev);
    /* A flag signals pending data; it does not queue samples or count IRQs. */
    atomic_set(&bmi_ready, 1);
}

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
    bool has_sample;   /* A timer tick permits a read; no sample is buffered yet. */
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
    int32_t bmi_sum[ACQ_BMI_AXES]; /* Wider sums prevent overflow over 5 samples. */
    /* Keep a complete packet while it crosses a storage-buffer boundary. */
    uint8_t packet[ACQ_PACKET_SIZE];
    uint8_t packet_remaining; /* Completed packet bytes still awaiting a copy. */
    uint8_t ecg_count;        /* ECG samples collected for the current packet. */
    uint8_t bmi_count;        /* Successful reads in the current average. */
    bool bmi_average_ready;  /* A fresh average is available in the packet. */
    bool timer_running;      /* Keep cadence when changing buffers without a pause. */
} acq_resources;

static int16_t read_ecg_signal(void);
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
    int err;

    /* Zephyr initializes the devices before main; only check readiness here. */
    if (!device_is_ready(ad8232_dev)) {
        return -ENODEV;
    }
    if (!device_is_ready(bmi323_dev)) {
        return -ENODEV;
    }

    err = bmi323_set_data_ready_callback(bmi323_dev, bmi_data_ready_callback);
    if (err != 0) {
        return err;
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

static int16_t read_ecg_signal(void)
{
    int16_t raw;
    int err = ad8232_read_raw(ad8232_dev, &raw);

    if (err != 0) {
        return INT16_MIN;
    }
    /* Return centered ADC counts, not microvolts; INT16_MIN marks a read error. */
    return raw - ECG_BASELINE_ADC;
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

static bool collect_bmi_sample(void)
{
    struct bmi323_sample bmi;

    /* Read and clear atomically so an ISR cannot interrupt a separate clear. */
    if (!atomic_set(&bmi_ready, 0)) {
        return true;
    }

    int err = bmi323_read_raw_sample(bmi323_dev, &bmi);

    if (err != 0) {
        printk("Acquisition: BMI read failed (%d)\n", err);
        memset(acq_resources.bmi_sum, 0, sizeof(acq_resources.bmi_sum));
        acq_resources.bmi_count = 0U;
        return false;
    }

    acq_resources.bmi_sum[0] += bmi.accel_x;
    acq_resources.bmi_sum[1] += bmi.accel_y;
    acq_resources.bmi_sum[2] += bmi.accel_z;
    acq_resources.bmi_sum[3] += bmi.gyro_x;
    acq_resources.bmi_sum[4] += bmi.gyro_y;
    acq_resources.bmi_sum[5] += bmi.gyro_z;
    acq_resources.bmi_count++;

    /* Five reads at 50 Hz produce one average at approximately 10 Hz. */
    if (acq_resources.bmi_count == ACQ_BMI_AVERAGE_SAMPLES) {
        if (acq_resources.bmi_average_ready) {
            printk("Acquisition: BMI average replaced\n");
        }
        for (size_t i = 0; i < ACQ_BMI_AXES; i++) {
            int16_t average = (int16_t)(acq_resources.bmi_sum[i] /
                                       (int32_t)ACQ_BMI_AVERAGE_SAMPLES);
            /* Skip the header and all 50 ECG values before writing each axis. */
            size_t offset = 1U + (ACQ_ECG_SAMPLES + i) * sizeof(int16_t);

            sys_put_le16((uint16_t)average, acq_resources.packet + offset);
            acq_resources.bmi_sum[i] = 0;
        }
        acq_resources.bmi_count = 0U;
        acq_resources.bmi_average_ready = true;
    }
    return true;
}

/* Copy only what fits; retain the remainder for the next buffer, without padding. */
static void copy_pending_packet(void)
{
    if (acq_resources.packet_remaining == 0U) {
        return;
    }

    size_t available = STORAGE_BLOCK_DATA_SIZE - acq_resources.write_pos;
    size_t length = acq_resources.packet_remaining;

    if (length > available) {
        length = available;
    }
    size_t offset = ACQ_PACKET_SIZE - acq_resources.packet_remaining;

    memcpy(acq_resources.active_buffer + acq_resources.write_pos,
           acq_resources.packet + offset, length);
    acq_resources.write_pos += length;
    acq_resources.packet_remaining -= length;

    if (acq_resources.packet_remaining == 0U) {
        /* Reuse packet memory only after every byte has been copied. */
        acq_resources.ecg_count = 0U;
    }
    acq_app_flags.buffer_ready =
        (acq_resources.write_pos == STORAGE_BLOCK_DATA_SIZE);
}

static void idle_handler(void)
{
    /* Reset handoff flags, but keep any packet bytes spanning two buffers. */
    acq_app_flags.has_buffer = false;
    acq_app_flags.has_sample = false;
    acq_app_flags.buffer_ready = false;
    acq_app_flags.done = false;
}

static void wait_buffer_handler(void)
{
    acq_app_flags.has_buffer = false;

    /* Try immediately so a normal buffer handoff does not restart the timer. */
    int err = k_msgq_get(acq_resources.free_queue,
                        &acq_resources.active_buffer, K_NO_WAIT);

    if (err != 0) {
        /* Preserve framing, but discard BMI accumulation across a pause. */
        k_timer_stop(&ecg_timer);
        acq_resources.timer_running = false;
        atomic_set(&bmi_ready, 0);
        memset(acq_resources.bmi_sum, 0, sizeof(acq_resources.bmi_sum));
        acq_resources.bmi_count = 0U;
        acq_resources.bmi_average_ready = false;

        /* No sensor reads while waiting; this pause creates a gap in time. */
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
    /* Finish the previous packet before collecting the next ECG sample. */
    copy_pending_packet();

    if (!acq_resources.timer_running) {
        atomic_set(&bmi_ready, 0);
        k_timer_start(&ecg_timer, K_MSEC(2), K_MSEC(2));
        acq_resources.timer_running = true;
    }
    acq_app_flags.has_buffer = true;
}

static void wait_sample_handler(void)
{
    /* Block until expiry; multiple expiries mean sampling deadlines were missed. */
    uint32_t ticks = k_timer_status_sync(&ecg_timer);

    acq_app_flags.has_sample = false;
    if (ticks > 1U) {
        printk("Acquisition: missed %u ticks\n", (unsigned int)(ticks - 1U));
        return;
    }
    acq_app_flags.has_sample = (ticks == 1U);
}

static void read_sample_handler(void)
{
    acq_app_flags.has_sample = false;

    int16_t ecg = read_ecg_signal();

    if (ecg == INT16_MIN) {
        printk("Acquisition: ECG read failed\n");
        return;
    }
    if (!collect_bmi_sample()) {
        return;
    }
    size_t offset = 1U + acq_resources.ecg_count * sizeof(int16_t);

    /* Count only successful reads; never publish an incomplete packet. */
    sys_put_le16((uint16_t)ecg, acq_resources.packet + offset);
    acq_resources.ecg_count++;

    if (acq_resources.ecg_count == ACQ_ECG_SAMPLES) {
        /* Finalize after 50 successful ECG samples. Errors extend this interval. */
        acq_resources.packet[0] = ACQ_PACKET_HEADER;
        if (!acq_resources.bmi_average_ready) {
            /* Discard this packet, but keep BMI accumulation for the next one. */
            acq_resources.ecg_count = 0U;
            printk("Acquisition: BMI average unavailable\n");
            return;
        }
        acq_resources.packet[ACQ_PACKET_SIZE - 1U] = ACQ_PACKET_TAIL;
        acq_resources.bmi_average_ready = false;
        acq_resources.packet_remaining = ACQ_PACKET_SIZE;
        copy_pending_packet();
    }
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

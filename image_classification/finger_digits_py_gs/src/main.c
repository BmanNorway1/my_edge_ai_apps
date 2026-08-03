/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/video.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <axon/nrf_axon_platform.h>
#include <drivers/axon/nrf_axon_driver.h>
#include <drivers/axon/nrf_axon_nn_infer.h>

#include "generated/nrf_axon_model_finger_digits_.h"  // your compiled model

#include "ble_nus.h"

LOG_MODULE_REGISTER(main);

#define CAM_WIDTH    96
#define CAM_HEIGHT   96
#define MODEL_WIDTH  96
#define MODEL_HEIGHT 96

#define NUM_CLASSES 7 //Finger digits 0-5 + unknown
#define CLASS_UNKNOWN 6

/* Consecutive identical predictions required before a digit is reported to the
 * central. At one capture per 500 ms this is ~2.5 s of holding the same pose,
 * which filters out the noisy frames while the hand is moving into place. */
#define STABLE_PREDICTIONS_REQUIRED 5

#define FRAME_RGB565_BYTES ((CAM_WIDTH) * (CAM_HEIGHT) * 2)

/* MUST match the data-collection firmware. AE/AGC needs time to settle; if
 * training frames are captured after N warmup frames but inference reads
 * frame 0, the two see different brightness -> train/serve mismatch.
 * The stream is start/stopped every capture here, so AE resets each time and
 * warmup matters just as much as it does during collection. Keep these equal. */
#define WARMUP_FRAMES 8

static const char * const finger_digit_labels[] = {
    "zero", "one", "two", "three", "four", "five", "unknown"
};

/* Wire tokens, in the same format game_controller uses for the snake commands:
 * one short uppercase ASCII token per command, CRLF-terminated, so that
 * game_receiver relays each one as a "Command: <token>" console line and the
 * host-side parser needs no new framing. "unknown" is never sent, hence NULL. */
static const char * const finger_digit_commands[] = {
    "ZERO\r\n", "ONE\r\n", "TWO\r\n", "THREE\r\n", "FOUR\r\n", "FIVE\r\n", NULL
};

BUILD_ASSERT(ARRAY_SIZE(finger_digit_commands) == NUM_CLASSES,
             "Mismatch between finger_digit_commands and NUM_CLASSES");
BUILD_ASSERT(ARRAY_SIZE(finger_digit_labels) == NUM_CLASSES,
             "Mismatch between finger_digit_labels and NUM_CLASSES");

/* Single-channel (grayscale) model input, input_shape=(H, W, 1).
 * NOTE: the pixels fed here are continuous grayscale (0..255 domain), NOT
 * binarized. The model MUST be retrained on grayscale images produced by the
 * exact same rgb565_to_gray() arithmetic used in the collection firmware. */
static int8_t input_buf[MODEL_WIDTH * MODEL_HEIGHT];
static int8_t output_buf[NUM_CLASSES];
static uint8_t gray_buf[MODEL_WIDTH * MODEL_HEIGHT];

/* Prediction debounce state. A digit is reported once its run of identical
 * predictions reaches STABLE_PREDICTIONS_REQUIRED, and not again until the
 * prediction changes -- so holding one pose emits exactly one command, and
 * showing the same digit twice means dropping the hand in between. */
static int stable_idx = -1;
static int stable_streak;
static bool stable_reported;

static const struct gpio_dt_spec led_capture = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec led_detection = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);

static void capture_timer_expiry(struct k_timer *timer);
static K_TIMER_DEFINE(capture_timer, capture_timer_expiry, NULL);
static K_SEM_DEFINE(capture_sem, 0, 1);

static void capture_timer_expiry(struct k_timer *timer)
{
    k_sem_give(&capture_sem);
}

static inline int8_t quantize(const float value, const nrf_axon_nn_compiled_model_input_s *in)
{
    const uint32_t quant_mult = in->quant_mult;
    const uint8_t quant_round = in->quant_round;
    const int8_t quant_zp = in->quant_zp;

    const float scale = (float)quant_mult / (float)(1 << quant_round);
    const int32_t quantized = (int32_t)(value * scale) + quant_zp;

    return (int8_t)__ssat(quantized, 8);
}

/*=============================== GRAYSCALE LUT ================================*/
static int8_t gray_lut[256];

static void init_gray_lut(const nrf_axon_nn_compiled_model_input_s *in)
{
    for (int v = 0; v < 256; v++) {
        gray_lut[v] = quantize((float)v, in);
    }
    LOG_INF("Gray LUT: [0]=%d [128]=%d [255]=%d",
            gray_lut[0], gray_lut[128], gray_lut[255]);
}
/*==============================================================================*/


static int led_init(const struct gpio_dt_spec *spec)
{
    int err;

    if (!gpio_is_ready_dt(spec)) {
        LOG_ERR("GPIO %s is not ready", spec->port->name);
        return -ENODEV;
    }

    err = gpio_pin_configure_dt(spec, GPIO_OUTPUT_INACTIVE);
    if (err) {
        LOG_ERR("Failed to configure %s pin %u (err %d)", spec->port->name, spec->pin, err);
        return err;
    }

    return 0;
}

/*============================ SHARED GRAYSCALE ================================
 * These two functions MUST be byte-for-byte identical to the data-collection
 * firmware, so the pixels the model sees at inference equal the pixels it was
 * trained on, exactly.
 *===========================================================================*/

static inline uint16_t extract_pixel(const uint8_t *data, const size_t pixel)
{
    const size_t offset = pixel * 2;

    return (uint16_t)((uint16_t)data[offset] << 8 | data[offset + 1]);
}

static inline uint8_t rgb565_to_gray(const uint16_t pixel)
{
    const uint8_t r5 = (pixel >> 11) & 0x1f;
    const uint8_t g6 = (pixel >> 5) & 0x3f;
    const uint8_t b5 = pixel & 0x1f;

    const uint8_t r = (r5 * 255) / 31;
    const uint8_t g = (g6 * 255) / 63;
    const uint8_t b = (b5 * 255) / 31;

    return (uint8_t)((r * 77 + g * 150 + b * 29) >> 8);
}

static void convert_chunk_to_model_input(const uint8_t *chunk_buf, const size_t pixel_start,
                                         const size_t pixel_count)
{
    for (size_t p = 0; p < pixel_count; p++) {
        /* CAM_WIDTH/HEIGHT == MODEL_WIDTH/HEIGHT, so pixels map 1:1 */
        gray_buf[pixel_start + p] = rgb565_to_gray(extract_pixel(chunk_buf, p));
    }
}
/*===========================================================================*/

static int capture_one_frame(const struct device *video)
{
    size_t total = 0;

    while (total < FRAME_RGB565_BYTES) {
        struct video_buffer *vbuf;
        int err = video_dequeue(video, &vbuf, K_FOREVER);

        if (err == -EAGAIN) {
            continue;
        }
        if (err) {
            LOG_ERR("video_dequeue failed: %d", err);
            return err;
        }

        const size_t room = FRAME_RGB565_BYTES - total;
        const size_t chunk = MIN(vbuf->bytesused, room);

        convert_chunk_to_model_input(vbuf->buffer, total / 2, chunk / 2);

        total += chunk;

        vbuf->type = VIDEO_BUF_TYPE_OUTPUT;
        video_enqueue(video, vbuf);
    }

    /* Grayscale -> quantized int8 via LUT (was: mean threshold to binary) */
    for (size_t i = 0; i < MODEL_WIDTH * MODEL_HEIGHT; i++) {
        input_buf[i] = gray_lut[gray_buf[i]];
    }

    return 0;
}

/* Feed one prediction into the debounce filter and, when it settles on a digit,
 * push the matching command to the central over NUS. */
static void report_prediction(const int class_idx)
{
    if (class_idx != stable_idx) {
        stable_idx = class_idx;
        stable_streak = 1;
        stable_reported = false;
    } else if (stable_streak < STABLE_PREDICTIONS_REQUIRED) {
        stable_streak++;
    }

    if (stable_reported || stable_streak < STABLE_PREDICTIONS_REQUIRED ||
        class_idx == CLASS_UNKNOWN) {
        return;
    }

    const char *cmd = finger_digit_commands[class_idx];
    int err = ble_nus_send(cmd, (uint16_t)strlen(cmd));

    if (err) {
        /* Not connected yet, or the stack pushed back. Stay unlatched so the
         * next frame of this same streak retries. */
        LOG_WRN("NUS send failed (err %d)", err);
        return;
    }

    stable_reported = true;
    LOG_INF("Sent command: %s", finger_digit_labels[class_idx]);
}

static int capture_and_classify(const struct device *video,
                                const nrf_axon_nn_compiled_model_s *model)
{
    int err;
    nrf_axon_result_e result;

    (void)gpio_pin_toggle_dt(&led_capture);

    err = video_stream_start(video, VIDEO_BUF_TYPE_OUTPUT);
    if (err) {
        LOG_ERR("Failed to start stream (err %d)", err);
        return -1;
    }

    /* Discard warmup frames so AE/AGC settles -- matches the collection
     * firmware, otherwise inference frames are brighter/darker than every
     * training image. */
    for (int i = 0; i < WARMUP_FRAMES; i++) {
        err = capture_one_frame(video);
        if (err) {
            LOG_ERR("Warmup frame failed (err %d)", err);
            (void)video_stream_stop(video, VIDEO_BUF_TYPE_OUTPUT);
            return -1;
        }
    }

    err = capture_one_frame(video);
    if (err) {
        LOG_ERR("Failed to capture frame (err %d)", err);
        (void)video_stream_stop(video, VIDEO_BUF_TYPE_OUTPUT);
        return -1;
    }

    err = video_stream_stop(video, VIDEO_BUF_TYPE_OUTPUT);
    if (err) {
        LOG_ERR("Failed to stop stream (err %d)", err);
        return -1;
    }

    result = nrf_axon_nn_model_infer_sync(model, input_buf, output_buf);
    if (result != NRF_AXON_RESULT_SUCCESS) {
        LOG_ERR("Inference failed (result %d)", result);
        return -1;
    }

    // Find the class with the highest score
    int8_t max_val = output_buf[0];
    int max_idx = 0;

    for (int i = 1; i < NUM_CLASSES; i++) {
        if (output_buf[i] > max_val) {
            max_val = output_buf[i];
            max_idx = i;
        }
    }

    // Calculate the score as a percentage based on the quantization parameters
    float max_score = (((float)max_val - model_finger_digits.output_dequant_zp)/256) * 100;
    LOG_INF("Detected finger digit: %s (score %d)", finger_digit_labels[max_idx], (int)max_score);

    report_prediction(max_idx);

    return 0;
}

int main(void)
{
    int err;
    nrf_axon_result_e result;

    const nrf_axon_nn_compiled_model_s *model = &model_finger_digits;
    const nrf_axon_nn_compiled_model_input_s *model_inputs =
        nrf_axon_nn_model_1st_external_input(model);

    const struct device *video = DEVICE_DT_GET(DT_NODELABEL(arducam_mega));
    struct video_buffer *vbufs[2];
    struct video_format fmt = {.type = VIDEO_BUF_TYPE_INPUT,
                               .pixelformat = VIDEO_PIX_FMT_RGB565,
                               .width = CAM_WIDTH,
                               .height = CAM_HEIGHT,
                               .pitch = CAM_WIDTH * 2};

    if (led_init(&led_capture) != 0) {
        return -1;
    }
    if (led_init(&led_detection) != 0) {
        return -1;
    }

    if (!device_is_ready(video)) {
        LOG_ERR("Video device not ready");
        return -1;
    }

    err = video_set_format(video, &fmt);
    if (err) {
        LOG_ERR("Setting video format failed (err %d)", err);
        return -1;
    }

    for (size_t i = 0; i < ARRAY_SIZE(vbufs); i++) {
        vbufs[i] = video_buffer_alloc(1024, K_NO_WAIT);
        if (vbufs[i] == NULL) {
            LOG_ERR("Allocation failed for video buffer %u", i);
            return -1;
        }
        vbufs[i]->type = VIDEO_BUF_TYPE_OUTPUT;
        video_enqueue(video, vbufs[i]);
    }

    result = nrf_axon_platform_init();
    if (result != NRF_AXON_RESULT_SUCCESS) {
        LOG_ERR("Axon platform init failed (result %d)", result);
        return -1;
    }

    result = nrf_axon_nn_model_validate(model);
    if (result != NRF_AXON_RESULT_SUCCESS) {
        LOG_ERR("Model validation failed (result %d)", result);
        return -1;
    }

    init_gray_lut(model_inputs);


    err = init_ble_nus();
    if (err) {
        LOG_ERR("BLE init failed (err %d)", err);
        return -1;
    }

    LOG_INF("Finger digit classifier start");

    k_timer_start(&capture_timer, K_NO_WAIT, K_MSEC(500));

    while (true) {
        err = k_sem_take(&capture_sem, K_FOREVER);
        if (err) {
            continue;
        }

        err = capture_and_classify(video, model);
        if (err) {
            return -1;
        }
    }

    return 0;
}
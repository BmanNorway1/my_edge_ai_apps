/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/video.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <axon/nrf_axon_platform.h>
#include <drivers/axon/nrf_axon_driver.h>
#include <drivers/axon/nrf_axon_nn_infer.h>

/* TODO: rename this include and the struct/macro references below (model_finger_digits_,
 * NRF_AXON_MODEL_*) once you have compiled your actual RGB model through
 * model_converter.py -> the Axon compiler. These names are placeholders. */
#include "generated/nrf_axon_model_finger_digits_.h"  // your compiled RGB model

#include "ble_nus.h"

LOG_MODULE_REGISTER(main);

#define CAM_WIDTH    96
#define CAM_HEIGHT   96
#define MODEL_WIDTH  96
#define MODEL_HEIGHT 96
#define MODEL_CHANNELS 3   /* RGB, planar (CHW) once quantized -- see below */

#define NUM_CLASSES 7 //Finger digits 0-5 + unknown

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

/*
 * 3-channel (RGB) model input, input_shape=(H, W, 3), trained with
 * COLOR_MODE = 'rgb' in train.py.
 *
 * NOTE: input_buf is stored PLANAR (CHW: all of R, then all of G, then all
 * of B), NOT interleaved (HWC: R,G,B per pixel). The camera naturally
 * produces interleaved data, and the Axon-compiled model expects planar
 * data -- this is the exact same layout mismatch that had to be patched
 * into the Edge Impulse SDK wrapper for RGB models on this platform. Since
 * this firmware is hand-written, the transpose is done correctly from the
 * start instead of needing a later patch. See the transpose loop in
 * capture_one_frame() below.
 *
 * The pixels fed here are continuous RGB (0..255 domain per channel), NOT
 * binarized. The model MUST be retrained on color images produced by the
 * exact same rgb565_to_rgb888() arithmetic used here (and in
 * collect_dataset.py's rgb565_to_png(), which this matches exactly).
 */
static int8_t input_buf[MODEL_WIDTH * MODEL_HEIGHT * MODEL_CHANNELS];
static int8_t output_buf[NUM_CLASSES];

/* Interleaved (HWC) staging buffer -- one R,G,B triplet per pixel, straight
 * from the camera, before the HWC -> CHW transpose into input_buf. */
static uint8_t rgb_buf[MODEL_WIDTH * MODEL_HEIGHT * MODEL_CHANNELS];

static int frame_count = 0;

static const struct gpio_dt_spec led_connected = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

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

/*=============================== QUANT LUT =====================================
 * ASSUMPTION: the compiled model uses a single shared scale/zero-point for
 * the whole input tensor (per-tensor quantization), NOT a separate one per
 * channel. This is the normal case for image inputs. If your compiled
 * model's header exposes per-channel quantization params instead, this
 * single LUT is wrong and each channel needs its own LUT -- check the
 * generated model header once you have it.
 *===============================================================================*/
static int8_t quant_lut[256];

static void init_quant_lut(const nrf_axon_nn_compiled_model_input_s *in)
{
    for (int v = 0; v < 256; v++) {
        quant_lut[v] = quantize((float)v, in);
    }
    LOG_INF("Quant LUT: [0]=%d [128]=%d [255]=%d",
            quant_lut[0], quant_lut[128], quant_lut[255]);
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

/*============================ SHARED RGB EXPANSION =============================
 * These functions MUST be byte-for-byte identical to the data-collection
 * pipeline (collect_dataset.py's rgb565_to_png()), so the pixels the model
 * sees at inference equal the pixels it was trained on, exactly. This
 * expansion formula (value * 255 // 31 or // 63) is the same one already
 * confirmed exact against that script for the grayscale firmware; RGB just
 * skips the luma-weighting step and keeps all three channels.
 *=================================================================================*/

static inline uint16_t extract_pixel(const uint8_t *data, const size_t pixel)
{
    const size_t offset = pixel * 2;

    return (uint16_t)((uint16_t)data[offset] << 8 | data[offset + 1]);
}

static inline void rgb565_to_rgb888(const uint16_t pixel, uint8_t *r8, uint8_t *g8, uint8_t *b8)
{
    const uint8_t r5 = (pixel >> 11) & 0x1f;
    const uint8_t g6 = (pixel >> 5) & 0x3f;
    const uint8_t b5 = pixel & 0x1f;

    *r8 = (uint8_t)((r5 * 255) / 31);
    *g8 = (uint8_t)((g6 * 255) / 63);
    *b8 = (uint8_t)((b5 * 255) / 31);
}

static void convert_chunk_to_model_input(const uint8_t *chunk_buf, const size_t pixel_start,
                                         const size_t pixel_count)
{
    for (size_t p = 0; p < pixel_count; p++) {
        uint8_t r8, g8, b8;

        rgb565_to_rgb888(extract_pixel(chunk_buf, p), &r8, &g8, &b8);

        /* CAM_WIDTH/HEIGHT == MODEL_WIDTH/HEIGHT, so pixels map 1:1.
         * Stored interleaved (HWC) here; transposed to planar (CHW) in
         * capture_one_frame() below, once the full frame has arrived. */
        const size_t pixel_idx = pixel_start + p;

        rgb_buf[pixel_idx * MODEL_CHANNELS + 0] = r8;
        rgb_buf[pixel_idx * MODEL_CHANNELS + 1] = g8;
        rgb_buf[pixel_idx * MODEL_CHANNELS + 2] = b8;
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

    /* HWC -> CHW transpose while quantizing. rgb_buf holds one interleaved
     * R,G,B triplet per pixel (camera-native order); input_buf needs all R
     * values, then all G values, then all B values, each already quantized
     * via the LUT. This is the step that has no grayscale equivalent --
     * with a single channel there was nothing to transpose. */
    const size_t plane = MODEL_WIDTH * MODEL_HEIGHT;

    for (size_t p = 0; p < plane; p++) {
        for (size_t c = 0; c < MODEL_CHANNELS; c++) {
            input_buf[c * plane + p] = quant_lut[rgb_buf[p * MODEL_CHANNELS + c]];
        }
    }

    return 0;
}

static int capture_and_classify(const struct device *video,
                                const nrf_axon_nn_compiled_model_s *model)
{
    int err;
    nrf_axon_result_e result;

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

    // Send predictions over BLE NUS
    frame_count++;
    if (frame_count % 2 == 0 && ble_nus_ready()) {
        char line[16];
        int n = snprintf(line, sizeof(line), "%s,%d\r\n", finger_digit_labels[max_idx], (int)max_score);
        if (n > 0 && n < (int)sizeof(line)) {
            ble_nus_send(line, n);
        }
    }

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

    if (led_init(&led_connected) != 0) {
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

    init_quant_lut(model_inputs);


    err = init_ble_nus(&led_connected);
    if (err) {
        LOG_ERR("BLE init failed (err %d)", err);
        return -1;
    }

    LOG_INF("Finger digit classifier start (RGB)");

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
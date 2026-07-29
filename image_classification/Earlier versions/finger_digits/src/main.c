/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <stdint.h>
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

LOG_MODULE_REGISTER(main);

#define CAM_WIDTH    128
#define CAM_HEIGHT   128
#define MODEL_WIDTH  64
#define MODEL_HEIGHT 64

#define NUM_CLASSES 7 //Finger digits 0-5 + unknown 
#define SCORE_THRESHOLD 90

#define FRAME_RGB565_BYTES ((CAM_WIDTH) * (CAM_HEIGHT) * 2)

#define LUT_SIZE_5_BITS 32
#define LUT_SIZE_6_BITS 64

static const char *finger_digit_labels[] = {
    "zero", "one", "two", "three", "four", "five", "unknown"
};

static int8_t input_buf[MODEL_WIDTH * MODEL_HEIGHT * 3];
static int8_t output_buf[NUM_CLASSES];
static uint8_t gray_buf[MODEL_WIDTH * MODEL_HEIGHT]; 

static int8_t lut_red_blue[LUT_SIZE_5_BITS];
static int8_t lut_green[LUT_SIZE_6_BITS];

static const struct gpio_dt_spec led_capture = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec led_detection = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);

static void capture_timer_expiry(struct k_timer *timer);
static K_TIMER_DEFINE(capture_timer, capture_timer_expiry, NULL);
static K_SEM_DEFINE(capture_sem, 0, 1);

static void capture_timer_expiry(struct k_timer *timer)
{
    k_sem_give(&capture_sem);
}

/*================================PRINTING IMAGE================================*/
static int frame_count = 0;
static void debug_print_input_buf(void)
{
    for (size_t row = 0; row < MODEL_HEIGHT; row++) {
        char line[MODEL_WIDTH + 1];
        for (size_t col = 0; col < MODEL_WIDTH; col++) {
            int8_t val = input_buf[0 * MODEL_WIDTH * MODEL_HEIGHT + row * MODEL_WIDTH + col];
            line[col] = (val > 0) ? '#' : '.';
        }
        line[MODEL_WIDTH] = '\0';
        LOG_INF("%s", line);
    }
}
/*==============================================================================*/


/*=============================Dynamic Thresholding=============================*/

static uint8_t mean_threshold(void)
{
    uint32_t sum = 0;
    const size_t count = MODEL_WIDTH * MODEL_HEIGHT;

    for (size_t i = 0; i < count; i++) {
        sum += gray_buf[i];
    }

    return (uint8_t)(sum / count);
}

static void apply_threshold_to_input_buf(const uint8_t threshold)
{
    const size_t count = MODEL_WIDTH * MODEL_HEIGHT;

    for (size_t i = 0; i < count; i++) {
        const int8_t binary = (gray_buf[i] > threshold) ? 127 : -128;

        input_buf[0 * count + i] = binary;
        input_buf[1 * count + i] = binary;
        input_buf[2 * count + i] = binary;
    }
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

static inline int8_t quantize(const float value, const nrf_axon_nn_compiled_model_input_s *in)
{
    const uint32_t quant_mult = in->quant_mult;
    const uint8_t quant_round = in->quant_round;
    const int8_t quant_zp = in->quant_zp;

    const float scale = (float)quant_mult / (float)(1 << quant_round);
    const int32_t quantized = (int32_t)(value * scale) + quant_zp;

    return (int8_t)__ssat(quantized, 8);
}

static void prefill_input_buf(const nrf_axon_nn_compiled_model_input_s *in)
{
    const float gray_symmetric = 0.0f;
    const int8_t gray = quantize(gray_symmetric, in);

    memset(input_buf, gray, sizeof(input_buf));
}

static void prefill_luts(const nrf_axon_nn_compiled_model_input_s *in)
{
    for (size_t i = 0; i < ARRAY_SIZE(lut_red_blue); i++) {
        const float value = (float)i / 32.f;
        const float value_sym = (value * 2.f) - 1.f;

        lut_red_blue[i] = quantize(value_sym, in);
    }

    for (size_t i = 0; i < ARRAY_SIZE(lut_green); i++) {
        const float value = (float)i / 64.f;
        const float value_sym = (value * 2.f) - 1.f;

        lut_green[i] = quantize(value_sym, in);
    }
}

static inline uint16_t extract_pixel(const uint8_t *data, const size_t pixel)
{
    const size_t offset = pixel * 2;

    return (uint16_t)((uint16_t)data[offset] << 8 | data[offset + 1]);
}

static void convert_chunk_to_model_input(const uint8_t *chunk_buf, const size_t pixel_start,
                                         const size_t pixel_count)
{
    for (size_t p = 0; p < pixel_count; p++) {
        const size_t pixel_idx = pixel_start + p;
        const size_t cam_row = pixel_idx / CAM_WIDTH;
        const size_t cam_col = pixel_idx % CAM_WIDTH;

        // Downsample 128x128 -> 64x64 by mapping each 2x2 block to one model pixel
        const size_t dst_row = cam_row / 2;
        const size_t dst_col = cam_col / 2;
        const size_t dst_offset = dst_row * MODEL_WIDTH + dst_col;

        const uint16_t pixel = extract_pixel(chunk_buf, p);
        const uint8_t r5 = (pixel >> 11) & 0x1f;
        const uint8_t g6 = (pixel >> 5) & 0x3f;
        const uint8_t b5 = pixel & 0x1f;

        // Convert RGB565 to grayscale
        uint8_t r = (r5 * 255) / 31;
        uint8_t g = (g6 * 255) / 63;
        uint8_t b = (b5 * 255) / 31;

        gray_buf[dst_offset] = (r * 77 + g * 150 + b * 29) >> 8;
    }
}

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
        const size_t chunk = min(vbuf->bytesused, room);

        convert_chunk_to_model_input(vbuf->buffer, total / 2, chunk / 2);

        total += chunk;

        vbuf->type = VIDEO_BUF_TYPE_OUTPUT;
        video_enqueue(video, vbuf);
    }

    const uint8_t threshold = mean_threshold();
    LOG_DBG("Mean threshold: %u", threshold);
    apply_threshold_to_input_buf(threshold);
 
    return 0;
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
    
    frame_count++;
    if(frame_count % 10 == 0) {
        LOG_INF("Captured frame %d", frame_count);
        debug_print_input_buf();
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
    // (max_val > SCORE_THRESHOLD) {
        LOG_INF("Detected finger digit: %s (score %d)", finger_digit_labels[max_idx], max_val);
            (void)gpio_pin_set_dt(&led_detection, 1);
    //else {
       //OG_INF("Unknown (max score %d)", max_val);
      //(void)gpio_pin_set_dt(&led_detection, 0);
    //

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

    prefill_input_buf(model_inputs);
    prefill_luts(model_inputs);

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
/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/video.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

/* Pulls in the impulse definition, run_classifier() and (via the Nordic Axon
 * inferencing engine) the model. All preprocessing — pixel scaling,
 * quantization to int8, output dequantization and label mapping — is handled
 * by the SDK, so this file only feeds raw pixels in and reads results out. */
#include "edge-impulse-sdk/classifier/ei_run_classifier.h"

#include "nordic-axon-model/nrf_axon_model_finger_digits_v2_5_test_vectors_.h"
#include <drivers/axon/nrf_axon_driver.h>

LOG_MODULE_REGISTER(main);

#define CAM_WIDTH  96
#define CAM_HEIGHT 96

#define FRAME_RGB565_BYTES ((CAM_WIDTH) * (CAM_HEIGHT) * 2)

/* Confidence needed to light the detection LED. */
#define DETECTION_THRESHOLD 0.6f

/* One captured frame, reassembled from the camera stream. The signal callback
 * reads from here on demand during run_classifier(). */
static uint8_t frame_rgb565[FRAME_RGB565_BYTES];

static const struct gpio_dt_spec led_capture = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec led_detection = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);

static void capture_timer_expiry(struct k_timer *timer);
static K_TIMER_DEFINE(capture_timer, capture_timer_expiry, NULL);
static K_SEM_DEFINE(capture_sem, 0, 1);

/* Fires every 500ms and signals the main loop to capture a new frame. */
static void capture_timer_expiry(struct k_timer *timer)
{
	k_sem_give(&capture_sem);
}

/* Configures a GPIO pin as an output and verifies it is ready. */
static int led_init(const struct gpio_dt_spec *spec)
{
	if (!gpio_is_ready_dt(spec)) {
		LOG_ERR("GPIO %s is not ready", spec->port->name);
		return -ENODEV;
	}

	int err = gpio_pin_configure_dt(spec, GPIO_OUTPUT_INACTIVE);
	if (err) {
		LOG_ERR("Failed to configure %s pin %u (err %d)", spec->port->name, spec->pin, err);
		return err;
	}

	return 0;
}

/*
 * Edge Impulse image signal callback.
 *
 * The "Image" DSP block expects one float per pixel, each holding an RGB888
 * value packed as 0xRRGGBB. The block unpacks and normalizes it (to the range
 * the model was trained on), and the Axon wrapper quantizes to int8 — none of
 * that is our concern here. We just unpack RGB565 -> RGB888 and pack the float.
 */
static int ei_camera_get_data(size_t offset, size_t length, float *out_ptr)
{
	for (size_t i = 0; i < length; i++) {
		const size_t px = offset + i;
		/* Same big-endian byte order the camera driver produces. */
		const uint16_t pixel =
			(uint16_t)((uint16_t)frame_rgb565[px * 2] << 8 | frame_rgb565[px * 2 + 1]);

		const uint8_t r5 = (pixel >> 11) & 0x1f;
		const uint8_t g6 = (pixel >> 5) & 0x3f;
		const uint8_t b5 = pixel & 0x1f;

		/* Scale each channel to full 8-bit range (max value -> 255). */
		const uint8_t r8 = (uint8_t)((r5 * 255 + 15) / 31);
		const uint8_t g8 = (uint8_t)((g6 * 255 + 31) / 63);
		const uint8_t b8 = (uint8_t)((b5 * 255 + 15) / 31);

		out_ptr[i] = (float)(((uint32_t)r8 << 16) | ((uint32_t)g8 << 8) | b8);
	}

	return EIDSP_OK;
}

/* Streams and reassembles a full RGB565 frame from the camera into frame_rgb565. */
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

		memcpy(&frame_rgb565[total], vbuf->buffer, chunk);
		total += chunk;

		vbuf->type = VIDEO_BUF_TYPE_OUTPUT;
		video_enqueue(video, vbuf);
	}

	return 0;
}

static int capture_and_classify(const struct device *video)
{
	int err;

	(void)gpio_pin_toggle_dt(&led_capture);

	err = video_stream_start(video, VIDEO_BUF_TYPE_OUTPUT);
	if (err) {
		LOG_ERR("Failed to start stream (err %d)", err);
		return -1;
	}

	err = capture_one_frame(video);
	(void)video_stream_stop(video, VIDEO_BUF_TYPE_OUTPUT);
	if (err) {
		LOG_ERR("Failed to capture frame (err %d)", err);
		return -1;
	}

	signal_t signal;
	signal.total_length = EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE; /* one float per pixel */
	signal.get_data = &ei_camera_get_data;

	ei_impulse_result_t result = {};
	EI_IMPULSE_ERROR ei_err = run_classifier(&signal, &result, false);
	if (ei_err != EI_IMPULSE_OK) {
		LOG_ERR("run_classifier failed (%d)", ei_err);
		return -1;
	}

	/* Labels and confidences come from the impulse; no manual label table. */
	int best_ix = -1;
	float best_val = 0.0f;
	for (size_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
		LOG_INF("  %-8s %d%%", result.classification[i].label,
			(int)(result.classification[i].value * 100.0f));
		if (result.classification[i].value > best_val) {
			best_val = result.classification[i].value;
			best_ix = (int)i;
		}
	}

	if (best_ix >= 0 && best_val >= DETECTION_THRESHOLD) {
		LOG_INF("Detected: %s (%d%%)", result.classification[best_ix].label,
			(int)(best_val * 100.0f));
		(void)gpio_pin_set_dt(&led_detection, 1);
	} else {
		LOG_INF("No confident detection (best %d%%)", (int)(best_val * 100.0f));
		(void)gpio_pin_set_dt(&led_detection, 0);
	}

	LOG_INF("timing: DSP %d ms, inference %d ms", result.timing.dsp,
		result.timing.classification);

	return 0;
}

static bool axon_selftest(void)
{
    bool all_pass = true;

    if (one_time_init() != EI_IMPULSE_OK) {
        LOG_ERR("SELFTEST: one_time_init failed");
        return false;
    }

    const size_t n = sizeof(finger_digits_v2_5_input_test_vectors) /
                      sizeof(finger_digits_v2_5_input_test_vectors[0]);

    for (size_t i = 0; i < n; i++) {
        nrf_axon_result_e r = nrf_axon_nn_model_infer_sync(
            nrf_axon_compiled_model,
            (int8_t *)finger_digits_v2_5_input_test_vectors[i],
            nrf_axon_compiled_model->packed_output_buf);

        if (r != NRF_AXON_RESULT_SUCCESS) {
            LOG_ERR("SELFTEST vector %u: inference failed (%d)", (unsigned)i, r);
            all_pass = false;
            continue;
        }

        const int8_t *actual = (const int8_t *)nrf_axon_compiled_model->packed_output_buf;
        const int8_t *expected = finger_digits_v2_5_expected_output_vectors[i];

        int actual_best = 0, expected_best = 0;
        for (int c = 1; c < EI_CLASSIFIER_LABEL_COUNT; c++) {
            if (actual[c] > actual[actual_best]) actual_best = c;
            if (expected[c] > expected[expected_best]) expected_best = c;
        }

        bool pass = (actual_best == expected_best);
        all_pass = all_pass && pass;
        LOG_INF("SELFTEST vector %u: expected=%s got=%s -> %s", (unsigned)i,
            ei_classifier_inferencing_categories[expected_best],
            ei_classifier_inferencing_categories[actual_best],
            pass ? "PASS" : "FAIL");
    }
    return all_pass;
}

int main(void)
{
	int err;

	if (!axon_selftest()) {
		LOG_ERR("Axon self-test FAILED -- do not trust live inference until fixed");
	} else {
		LOG_INF("Axon self-test PASSED");
	}

	const struct device *video = DEVICE_DT_GET(DT_NODELABEL(arducam_mega));
	struct video_buffer *vbufs[2];
	struct video_format fmt = {.type = VIDEO_BUF_TYPE_INPUT,
				   .pixelformat = VIDEO_PIX_FMT_RGB565,
				   .width = CAM_WIDTH,
				   .height = CAM_HEIGHT,
				   .pitch = CAM_WIDTH * 2};

	if (led_init(&led_capture) != 0 || led_init(&led_detection) != 0) {
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

	LOG_INF("Finger digit classifier start (%d classes, %dx%d)",
		EI_CLASSIFIER_LABEL_COUNT, EI_CLASSIFIER_INPUT_WIDTH, EI_CLASSIFIER_INPUT_HEIGHT);

	k_timer_start(&capture_timer, K_NO_WAIT, K_MSEC(500));

	while (true) {
		if (k_sem_take(&capture_sem, K_FOREVER) != 0) {
			continue;
		}
		(void)capture_and_classify(video);
	}

	return 0;
}

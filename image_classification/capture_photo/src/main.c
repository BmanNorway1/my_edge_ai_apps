/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

 // Arducam MEGA 3mp B0400:
 // wakeup time 42 ms
 // max resolution 2048x1536

#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/video.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <SEGGER_RTT.h>

LOG_MODULE_REGISTER(main);

#define CAM_WIDTH    96
#define CAM_HEIGHT   96

/* Raw RGB565 frame size -- exactly what the camera outputs, no markers */
#define FRAME_RGB565_BYTES ((CAM_WIDTH) * (CAM_HEIGHT) * 2)

#define RTT_SINGLE_CHANNEL 1     /* single-shot photos (sw0) */
#define RTT_STREAM_CHANNEL 2     /* stream-mode photos (sw1) */

#define WARMUP_FRAMES 8

#define STREAM_MAX_IMAGES 5
#define STREAM_INTERVAL_MS 1000

#define BUTTON_DEBOUNCE_MS 200

/* Frame buffer -- sized for one raw RGB565 frame, not a JPEG */
static uint8_t frame_buf[FRAME_RGB565_BYTES];
static uint8_t rtt_single_buf[4096];
static uint8_t rtt_stream_buf[4096];

static const struct gpio_dt_spec led_capture = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

static const struct gpio_dt_spec btn_capture = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static const struct gpio_dt_spec btn_stream   = GPIO_DT_SPEC_GET(DT_ALIAS(sw1), gpios);
static const struct gpio_dt_spec btn_stop     = GPIO_DT_SPEC_GET(DT_ALIAS(sw2), gpios);
static struct gpio_callback btn_capture_cb;
static struct gpio_callback btn_stream_cb;
static struct gpio_callback btn_stop_cb;

enum capture_mode {
	MODE_NONE = 0,
	MODE_SINGLE,
	MODE_STREAM,
};
static atomic_t pending_mode = ATOMIC_INIT(MODE_NONE);
static K_SEM_DEFINE(action_sem, 0, 1);
static atomic_t stop_requested = ATOMIC_INIT(0);

static bool debounce_ok(uint32_t *last)
{
	uint32_t now = k_uptime_get_32();

	if (now - *last < BUTTON_DEBOUNCE_MS) {
		return false;
	}
	*last = now;
	return true;
}

static void btn_capture_pressed(const struct device *port, struct gpio_callback *cb,
				uint32_t pins)
{
	static uint32_t last;

	if (!debounce_ok(&last)) {
		return;
	}
	atomic_set(&pending_mode, MODE_SINGLE);
	k_sem_give(&action_sem);
}

static void btn_stream_pressed(const struct device *port, struct gpio_callback *cb,
			       uint32_t pins)
{
	static uint32_t last;

	if (!debounce_ok(&last)) {
		return;
	}
	atomic_set(&pending_mode, MODE_STREAM);
	k_sem_give(&action_sem);
}

static void btn_stop_pressed(const struct device *port, struct gpio_callback *cb,
			     uint32_t pins)
{
	static uint32_t last;

	if (!debounce_ok(&last)) {
		return;
	}
	atomic_set(&stop_requested, 1);
}

static int led_init(const struct gpio_dt_spec *spec)
{
	int err;

	if (!gpio_is_ready_dt(spec)) {
		LOG_ERR("GPIO %s is not ready", spec->port->name);
		return -ENODEV;
	}
	err = gpio_pin_configure_dt(spec, GPIO_OUTPUT_INACTIVE);
	if (err) {
		LOG_ERR("Failed to configure %s pin %u (err %d)",
			spec->port->name, spec->pin, err);
		return err;
	}
	return 0;
}

static int button_init(const struct gpio_dt_spec *spec, struct gpio_callback *cb,
		       gpio_callback_handler_t handler)
{
	int err;

	if (!gpio_is_ready_dt(spec)) {
		LOG_ERR("GPIO %s is not ready", spec->port->name);
		return -ENODEV;
	}
	err = gpio_pin_configure_dt(spec, GPIO_INPUT);
	if (err) {
		LOG_ERR("Failed to configure %s pin %u (err %d)",
			spec->port->name, spec->pin, err);
		return err;
	}
	err = gpio_pin_interrupt_configure_dt(spec, GPIO_INT_EDGE_TO_ACTIVE);
	if (err) {
		LOG_ERR("Failed to configure interrupt on %s pin %u (err %d)",
			spec->port->name, spec->pin, err);
		return err;
	}
	gpio_init_callback(cb, handler, BIT(spec->pin));
	err = gpio_add_callback(spec->port, cb);
	if (err) {
		LOG_ERR("Failed to add callback on %s (err %d)", spec->port->name, err);
		return err;
	}
	return 0;
}

/* Collect exactly one RGB565 frame into out[0..FRAME_RGB565_BYTES-1].
 * RGB565 is fixed-size so no marker scanning is needed -- just accumulate
 * bytes until the frame is full.
 */
static int capture_one_frame(const struct device *video, uint8_t *out)
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

		const size_t room  = FRAME_RGB565_BYTES - total;
		const size_t chunk = MIN(vbuf->bytesused, room);

		memcpy(&out[total], vbuf->buffer, chunk);
		total += chunk;

		vbuf->type = VIDEO_BUF_TYPE_OUTPUT;
		video_enqueue(video, vbuf);
	}

	return 0;
}

static int capture_and_store(const struct device *video)
{
	int err;

	(void)gpio_pin_set_dt(&led_capture, 1);

	err = video_stream_start(video, VIDEO_BUF_TYPE_OUTPUT);
	if (err) {
		LOG_ERR("Failed to start stream (err %d)", err);
		(void)gpio_pin_set_dt(&led_capture, 0);
		return -1;
	}

	/* Discard warmup frames so auto-exposure/gain can settle */
	for (int i = 0; i < WARMUP_FRAMES; i++) {
		err = capture_one_frame(video, frame_buf);
		if (err) {
			LOG_ERR("Warmup frame failed (err %d)", err);
			(void)video_stream_stop(video, VIDEO_BUF_TYPE_OUTPUT);
			(void)gpio_pin_set_dt(&led_capture, 0);
			return -1;
		}
	}

	/* Capture the actual frame to send */
	err = capture_one_frame(video, frame_buf);
	if (err) {
		LOG_ERR("Failed to capture frame (err %d)", err);
		(void)video_stream_stop(video, VIDEO_BUF_TYPE_OUTPUT);
		(void)gpio_pin_set_dt(&led_capture, 0);
		return -1;
	}

	(void)video_stream_stop(video, VIDEO_BUF_TYPE_OUTPUT);

	/* Send the raw RGB565 frame -- Python converts it to PNG */
	SEGGER_RTT_Write(RTT_SINGLE_CHANNEL, frame_buf, FRAME_RGB565_BYTES);
	LOG_INF("Sent %u-byte RGB565 frame over RTT channel %d",
		FRAME_RGB565_BYTES, RTT_SINGLE_CHANNEL);

	(void)gpio_pin_set_dt(&led_capture, 0);
	return 0;
}

static void interruptible_sleep(int ms)
{
	const int step = 50;

	for (int elapsed = 0; elapsed < ms; elapsed += step) {
		if (atomic_get(&stop_requested)) {
			return;
		}
		k_msleep(MIN(step, ms - elapsed));
	}
}

static void stream_capture(const struct device *video)
{
	int err;

	atomic_clear(&stop_requested);
	(void)gpio_pin_set_dt(&led_capture, 1);

	err = video_stream_start(video, VIDEO_BUF_TYPE_OUTPUT);
	if (err) {
		LOG_ERR("Failed to start stream (err %d)", err);
		(void)gpio_pin_set_dt(&led_capture, 0);
		return;
	}

	/* Warm up once; stream stays running so AE/AGC stays settled */
	for (int i = 0; i < WARMUP_FRAMES; i++) {
		if (atomic_get(&stop_requested)) {
			goto out;
		}
		err = capture_one_frame(video, frame_buf);
		if (err) {
			LOG_ERR("Warmup frame failed (err %d)", err);
			goto out;
		}
	}

	LOG_INF("Stream mode: capturing %d images", STREAM_MAX_IMAGES);

	for (int n = 0; n < STREAM_MAX_IMAGES; n++) {
		if (atomic_get(&stop_requested)) {
			LOG_INF("Stream stopped by user after %d image(s)", n);
			break;
		}

		err = capture_one_frame(video, frame_buf);
		if (err) {
			LOG_ERR("Stream frame %d failed (err %d), stopping", n + 1, err);
			break;
		}

		/* Send raw RGB565 frame -- Python converts it to PNG */
		SEGGER_RTT_Write(RTT_STREAM_CHANNEL, frame_buf, FRAME_RGB565_BYTES);
		LOG_INF("Stream %d/%d: sent %u bytes over RTT channel %d",
			n + 1, STREAM_MAX_IMAGES, FRAME_RGB565_BYTES, RTT_STREAM_CHANNEL);

		if (n + 1 < STREAM_MAX_IMAGES) {
			interruptible_sleep(STREAM_INTERVAL_MS);
		}
	}

out:
	(void)video_stream_stop(video, VIDEO_BUF_TYPE_OUTPUT);
	(void)gpio_pin_set_dt(&led_capture, 0);
	atomic_clear(&stop_requested);
	LOG_INF("Stream mode done");
}

int main(void)
{
	int err;

	const struct device *video = DEVICE_DT_GET(DT_NODELABEL(arducam_mega));
	struct video_buffer *vbufs[2];

	/* Match the inference firmware exactly -- RGB565, same pitch */
	struct video_format fmt = {
		.type        = VIDEO_BUF_TYPE_INPUT,
		.pixelformat = VIDEO_PIX_FMT_RGB565,
		.width       = CAM_WIDTH,
		.height      = CAM_HEIGHT,
		.pitch       = CAM_WIDTH * 2,
	};

	if (led_init(&led_capture) != 0) {
		return -1;
	}
	if (button_init(&btn_capture, &btn_capture_cb, btn_capture_pressed) != 0) {
		return -1;
	}
	if (button_init(&btn_stream, &btn_stream_cb, btn_stream_pressed) != 0) {
		return -1;
	}
	if (button_init(&btn_stop, &btn_stop_cb, btn_stop_pressed) != 0) {
		return -1;
	}

	if (!device_is_ready(video)) {
		LOG_ERR("Video device not ready");
		return -1;
	}

	/* Name the RTT channels to reflect the new format */
	SEGGER_RTT_ConfigUpBuffer(RTT_SINGLE_CHANNEL, "RGB565_SINGLE",
				  rtt_single_buf, sizeof(rtt_single_buf),
				  SEGGER_RTT_MODE_BLOCK_IF_FIFO_FULL);
	SEGGER_RTT_ConfigUpBuffer(RTT_STREAM_CHANNEL, "RGB565_STREAM",
				  rtt_stream_buf, sizeof(rtt_stream_buf),
				  SEGGER_RTT_MODE_BLOCK_IF_FIFO_FULL);

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

	while (true) {
		LOG_INF("Idle: sw0 = single photo, sw1 = stream mode, sw2 = stop stream");

		if (k_sem_take(&action_sem, K_FOREVER) != 0) {
			continue;
		}

		switch (atomic_set(&pending_mode, MODE_NONE)) {
		case MODE_SINGLE:
			err = capture_and_store(video);
			if (err) {
				LOG_WRN("Capture failed (err %d), skipping this shot", err);
			}
			break;
		case MODE_STREAM:
			stream_capture(video);
			break;
		default:
			break;
		}
	}
	return 0;
}
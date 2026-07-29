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

#include <nrf_axon_model_coffee_cup_vs_banana_10_.h> 

LOG_MODULE_REGISTER(main);

#define CAM_WIDTH    96
#define CAM_HEIGHT   96
#define MODEL_WIDTH  96
#define MODEL_HEIGHT 96
#define PAD_LEFT     (((MODEL_WIDTH) - (CAM_WIDTH)) / 2)
#define PAD_TOP	     (((MODEL_HEIGHT) - (CAM_HEIGHT)) / 2)

#define NUM_CLASSES 3 // Coffee and banana + unknown 

#define FRAME_RGB565_BYTES ((CAM_WIDTH) * (CAM_HEIGHT) * 2) //Each pixel is 2 bytes in RGB565 format

#define LUT_SIZE_5_BITS 32
#define LUT_SIZE_6_BITS 64

static int8_t input_buf[MODEL_WIDTH * MODEL_HEIGHT * 3];
static int8_t output_buf[NUM_CLASSES];

static int8_t lut_red_blue[LUT_SIZE_5_BITS];
static int8_t lut_green[LUT_SIZE_6_BITS];

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

/* Converts a float in [-1, 1] to a quantized int8 using the model's scale and zero-point. */
static inline int8_t quantize(const float value, const nrf_axon_nn_compiled_model_input_s *in)
{
	const uint32_t quant_mult = in->quant_mult;
	const uint8_t quant_round = in->quant_round;
	const int8_t quant_zp = in->quant_zp;

	const float scale = (float)quant_mult / (float)(1 << quant_round);
	const int32_t quantized = (int32_t)(value * scale) + quant_zp;

	return (int8_t)__ssat(quantized, 8);
}

/* Fills the entire input buffer with the quantized value of 0.0 (mid-gray). */
static void prefill_input_buf(const nrf_axon_nn_compiled_model_input_s *in)
{
	const float gray_symmetric = 0.0f;
	const int8_t gray = quantize(gray_symmetric, in);

	memset(input_buf, gray, sizeof(input_buf));
}

/* Precomputes quantized int8 lookup tables for RGB565 pixel values (5-bit R/B, 6-bit G). */
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

/* Reads one big-endian RGB565 pixel from a byte buffer at the given pixel index. */
static inline uint16_t extract_pixel(const uint8_t *data, const size_t pixel)
{
	const size_t offset = pixel * 2;

	return (uint16_t)((uint16_t)data[offset] << 8 | data[offset + 1]);
}

/* Converts a chunk of RGB565 pixels to quantized int8 and writes them into the
   model input buffer, padding if needed. */
static void convert_chunk_to_model_input(const uint8_t *chunk_buf, const size_t pixel_start,
					 const size_t pixel_count)
{
	for (size_t p = 0; p < pixel_count; p++) {
		const size_t pixel_idx = pixel_start + p;
		const size_t cam_row = pixel_idx / CAM_WIDTH;
		const size_t cam_col = pixel_idx % CAM_WIDTH;
		const size_t model_row = PAD_TOP + cam_row;
		const size_t model_col = PAD_LEFT + cam_col;
		const size_t dst_offset = model_row * MODEL_WIDTH + model_col;

		const uint16_t pixel = extract_pixel(chunk_buf, p);
		const uint8_t r5 = (pixel >> 11) & 0x1f;
		const uint8_t g6 = (pixel >> 5) & 0x3f;
		const uint8_t b5 = pixel & 0x1f;

		input_buf[0 * MODEL_WIDTH * MODEL_HEIGHT + dst_offset] = (int8_t)lut_red_blue[r5];
		input_buf[1 * MODEL_WIDTH * MODEL_HEIGHT + dst_offset] = (int8_t)lut_green[g6];
		input_buf[2 * MODEL_WIDTH * MODEL_HEIGHT + dst_offset] = (int8_t)lut_red_blue[b5];
	}
}

/* Streams and reassembles a full RGB565 frame from the camera into the model input buffer. */
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

	return 0;
}


/* Captures one frame, runs inference, and toggles LEDs and logs based on detections. */
static int capture_and_detect(const struct device *video, const nrf_axon_nn_compiled_model_s *model)
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

	result = nrf_axon_nn_model_infer_sync(model, input_buf, output_buf);
	if (result != NRF_AXON_RESULT_SUCCESS) {
		LOG_ERR("Inference failed (result %d)", result);
		(void)gpio_pin_set_dt(&led_detection, 0);
		return -1;
	}

	/* Print inference results*/
	for (int i = 0; i < NUM_CLASSES; i++) {
		LOG_INF("Class %d: score %d", i, output_buf[i]);
	}

	return 0;
}

/* Initialises camera, LEDs, Axon platform and model, then runs coffee vs banana detection every 500ms. */
int main(void)
{
	int err;

	nrf_axon_result_e result;
	const nrf_axon_nn_compiled_model_s *model = &model_coffee_cup_vs_banana_10;
	const nrf_axon_nn_compiled_model_input_s *model_inputs = nrf_axon_nn_model_1st_external_input(model);

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


	LOG_INF("Coffee vs banana start");

	k_timer_start(&capture_timer, K_NO_WAIT, K_MSEC(500));

	while (true) {
		err = k_sem_take(&capture_sem, K_FOREVER);
		if (err) {
			continue;
		}

		err = capture_and_detect(video, model);
		if (err) {
			return -1;
		}
	}

	return 0;
}

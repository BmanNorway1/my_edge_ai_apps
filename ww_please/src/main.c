/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <stddef.h>
#include <stdint.h>

#include <zephyr/audio/dmic.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>

#include "control_output.h"
#include "dmic.h"
#include "kws/kws.h"
#include "leds.h"
#include "ww/wakeword.h"

LOG_MODULE_REGISTER(main);

#define DMIC_READ_TIMEOUT 100
#define BUTTON_DEBOUNCE_MS 200

static const struct device *const dmic_dev = DEVICE_DT_GET(DT_NODELABEL(dmic_dev));
static const struct device *const button_dev = DEVICE_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static struct gpio_callback btn_turnonpin_cb;

// njb
#define CONFIG_WW_TIMEOUT_MS 3000
static const struct gpio_dt_spec dispenser = GPIO_DT_SPEC_GET(DT_ALIAS(ww_out), gpios);

static bool debounce_ok(uint32_t *last)
{
	uint32_t now = k_uptime_get_32();

	if (now - *last < BUTTON_DEBOUNCE_MS) {
		return false;
	}
	*last = now;
	return true;
}

static void btn_trunonpin_pressed(const struct device *port, struct gpio_callback *cb,
				uint32_t pins)
{
	static uint32_t last;

	if (!debounce_ok(&last)) {
		return;
	}
	gpio_pin_set_dt(&dispenser, 1);
	leds_on_led0();
	k_sleep(K_MSEC(1000)); 			
	gpio_pin_set_dt(&dispenser, 0);
	leds_off_led0();
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


static int ww_loop(void)
{
	int err;
	void *audio_buffer;
	size_t audio_buffer_size;
	bool ww_detected;

	ww_reset();

	print_control_output((struct control_message){CONTROL_MESSAGE_WAITING_WW});
	
	while (true) {
		err = dmic_read(dmic_dev, 0, &audio_buffer, &audio_buffer_size, DMIC_READ_TIMEOUT);
		if (err < 0) {
			LOG_ERR("Failed to read from DMIC (err %d)", err);
			return err; 
		}

		err = ww_process(audio_buffer, DMIC_SAMPLES_IN_BLOCK, &ww_detected);
		if (err == -EBUSY) {
			/* More data is needed. */
			continue;
		} else if (err < 0) {
			LOG_ERR("Wakeword detection failed (err %d)", err);
			return err;
		}

		if (ww_detected) {
			print_control_output((struct control_message){CONTROL_MESSAGE_WW_DETECTED});
			//MIGHT WANT TO CHANGE THIS FOR NON-BLOCKING BEHAVIOR
			gpio_pin_set_dt(&dispenser, 1);
			leds_on_led0();
			k_sleep(K_MSEC(1000)); 
			
			gpio_pin_set_dt(&dispenser, 0);
			leds_off_led0();

			return 0;
		}
	}
}

static int kws_loop(void)
{
	int err;
	void *audio_buffer;
	size_t audio_buffer_size;
	struct kws_prediction prediction;

	uint32_t spotting_timeout = k_uptime_get_32() + CONFIG_KWS_PERIOD_MS;

	kws_reset();

	print_control_output((struct control_message){.type = CONTROL_MESSAGE_WAITING_KW});

	while (IS_ENABLED(CONFIG_APP_MODE_KWS_ONLY) || spotting_timeout > k_uptime_get_32()) {
		err = dmic_read(dmic_dev, 0, &audio_buffer, &audio_buffer_size, DMIC_READ_TIMEOUT);
		if (err < 0) {
			LOG_ERR("Failed to read from DMIC (err %d)", err);
			return err;
		}

		err = kws_process(audio_buffer, DMIC_SAMPLES_IN_BLOCK, &prediction);
		if (err == -EBUSY) {
			/* More data is needed. */
			continue;
		} else if (err) {
			LOG_ERR("Keyword spotting failed (err %d)", err);
			return err;
		}

		if (prediction.valid) {
			leds_blink_led1();
			spotting_timeout = k_uptime_get_32() + CONFIG_KWS_PERIOD_MS;
			print_control_output(
				(struct control_message){.type = CONTROL_MESSAGE_KW_SPOTTED,
							 .kw_class = prediction.class,
							 .name = prediction.name});
		}
	}

	print_control_output((struct control_message){.type = CONTROL_MESSAGE_TIMEOUT_KWS});

	return 0;
}

int main(void)
{
	int err;

	err = dmic_init();
	if (err) {
		return err;
	}

	err = leds_init();
	if (err) {
		return err;
	}

	err = control_output_init();
	if (err) {
		return err;
	}

	err = ww_init();
	if (err) {
		return err;
	}

	err = kws_init();
	if (err) {
		return err;
	}

	if (!gpio_is_ready_dt(&dispenser)) {
        return -ENODEV;
    }
    gpio_pin_configure_dt(&dispenser, GPIO_OUTPUT_INACTIVE);

	if (button_init(&btn_turnonpin, &btn_turnonpin_cb, btn_turnonpin_pressed) != 0) {
		return -1;
	}

	LOG_INF("Initialization completed, check output on VCOM0");

	err = dmic_trigger(dmic_dev, DMIC_TRIGGER_START);
	if (err < 0) {
		LOG_ERR("Failed to start DMIC (err %d)", err);
		return err;
	}

	while (true) {
		if (IS_ENABLED(CONFIG_APP_MODE_WW_GATED_KWS) ||
		    IS_ENABLED(CONFIG_APP_MODE_WW_ONLY)) {
			err = ww_loop();
			if (err) {
				return err;
			}
		}

		if (IS_ENABLED(CONFIG_APP_MODE_WW_GATED_KWS)) {
			leds_on_led0();
		}

		if (IS_ENABLED(CONFIG_APP_MODE_WW_GATED_KWS) ||
		    IS_ENABLED(CONFIG_APP_MODE_KWS_ONLY)) {
			err = kws_loop();
			if (err) {
				return err;
			}
		}

		if (IS_ENABLED(CONFIG_APP_MODE_WW_GATED_KWS)) {
			leds_off_led0();
		}
	}

	return 0;
}

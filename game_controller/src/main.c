/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * @brief Game controller entry point.
 *
 * Brings up the inference engines and the button, then hands control to the
 * engine controller. A short button click switches between engines (KWS and
 * gesture recognition); each engine owns its own model and acquisition path.
 */

#include <zephyr/logging/log.h>

#include "ble/ble_nus.h"
#include "hw_modules/button.h"
#include "engine/engine_controller.h"

LOG_MODULE_REGISTER(main);

static void on_button_click(button_click_t click)
{
	if (click == BUTTON_CLICK_SHORT) {
		engine_request_switch();
	} else {
		LOG_INF("Long click (no action yet)");
	}
}

int main(void)
{
	int err;

	err = engine_controller_init();
	if (err) {
		LOG_ERR("Engine controller init failed (err %d)", err);
		return err;
	}

	err = button_init();
	if (err) {
		LOG_ERR("Button init failed (err %d)", err);
		return err;
	}
	button_reg_click_handler(on_button_click);

	err = ble_nus_init();
	if (err) {
		LOG_ERR("BLE NUS init failed (err %d)", err);
		return err;
	}

	/* 	Runs the active engine and switches between them on request
		Function is a while loop. */
	engine_controller_run();

	return 0;
}

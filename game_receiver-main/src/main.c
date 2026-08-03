/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * @brief Game receiver entry point.
 *
 * Bridges the game_controller's BLE commands onto this board's console
 * UART: connects as a NUS central and relays every received token as a
 * "Command: <token>" console line.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "ble/ble_central.h"

LOG_MODULE_REGISTER(main);

int main(void)
{
	int err;

	err = ble_central_init();
	if (err) {
		LOG_ERR("BLE central init failed (err %d)", err);
		return err;
	}

	return 0;
}

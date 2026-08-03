/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/**
 * @defgroup ble_nus Bluetooth NUS interface
 * @{
 * @ingroup game_controller
 *
 * @brief Sends game controller commands to a connected central over the
 *        Nordic UART Service (NUS).
 *
 * Commands are sent as short ASCII tokens (e.g. "UP\r\n") so they can be read
 * directly in the nRF Connect for Mobile NUS console during bring-up. A
 * dedicated central can parse the same tokens.
 */
#ifndef __BLE_NUS_H__
#define __BLE_NUS_H__

#include "ble/game_command.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * @brief Initialize the BLE stack and NUS, then start advertising.
 *
 * @return 0 on success, negative errno on failure.
 */
int ble_nus_init(void);

/**
 * @brief Send a game controller command to the connected central.
 *
 * @param cmd Command to send. @ref GAME_CMD_NONE is rejected.
 *
 * @retval 0 on success.
 * @retval -EINVAL if @p cmd is not a sendable command.
 * @retval -ENOTCONN if no central is connected or notifications are disabled.
 * @retval other negative errno from the BLE stack.
 */
int ble_nus_send_command(enum game_command cmd);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* __BLE_NUS_H__ */

/**
 * @}
 */

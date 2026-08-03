/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/**
 * @defgroup game_command Game controller command set
 * @{
 * @ingroup game_controller
 *
 * @brief Transport-agnostic command set shared by the inference engines and
 *        the BLE layer. Each engine maps its model classes to one of these
 *        commands; the BLE layer maps commands to its wire format.
 */
#ifndef __GAME_COMMAND_H__
#define __GAME_COMMAND_H__

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * @brief Directional command emitted by an inference engine.
 *
 * @c GAME_CMD_NONE means "no command" (e.g. a recognized but non-directional
 * class) and is never sent over the wire.
 */
enum game_command {
	GAME_CMD_NONE = 0,
	GAME_CMD_UP,
	GAME_CMD_DOWN,
	GAME_CMD_LEFT,
	GAME_CMD_RIGHT,
};

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* __GAME_COMMAND_H__ */

/**
 * @}
 */

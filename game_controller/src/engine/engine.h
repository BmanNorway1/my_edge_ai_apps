/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/**
 * @defgroup engine Inference engine interface
 * @{
 * @ingroup game_controller
 *
 * @brief Polymorphic interface for a self-contained inference engine.
 *
 * Each engine owns one model and its acquisition path (e.g. KWS over the DMIC,
 * gesture over the IMU). The engine controller activates one engine at a time
 * and drives its life cycle:
 *
 *   init()  - once at startup: bring up hardware and the model.
 *   enter() - on becoming active: reset model state and start acquisition.
 *   run()   - loop acquisition + inference until the stop flag is set.
 *   exit()  - on becoming inactive: stop acquisition.
 */
#ifndef __ENGINE_H__
#define __ENGINE_H__

#include <zephyr/sys/atomic.h>

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

typedef struct engine {
	/** Human-readable engine name, for logging. */
	const char *name;

	/** One-time hardware + model initialization. Returns 0 on success. */
	int (*init)(void);

	/** Called when the engine becomes active. Returns 0 on success. */
	int (*enter)(void);

	/** Called when the engine becomes inactive. */
	void (*exit)(void);

	/**
	 * @brief Run the engine's acquisition + inference loop.
	 *
	 * Must return once @p stop becomes non-zero (checked via atomic_get).
	 *
	 * @param stop  Stop flag owned by the controller.
	 */
	void (*run)(atomic_t *stop);
} engine_t;

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* __ENGINE_H__ */

/**
 * @}
 */

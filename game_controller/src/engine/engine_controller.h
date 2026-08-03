/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/**
 * @defgroup engine_controller Inference engine controller
 * @{
 * @ingroup game_controller
 *
 * @brief Owns the set of inference engines, runs the active one, and switches
 *        between them on request.
 */
#ifndef __ENGINE_CONTROLLER_H__
#define __ENGINE_CONTROLLER_H__

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * @brief Initialize all registered engines (one-time hardware + model init).
 *
 * @return 0 on success, negative error code from the first failing engine.
 */
int engine_controller_init(void);

/**
 * @brief Run the active engine, switching to the next on request.
 *
 * Does not return.
 */
void engine_controller_run(void);

/**
 * @brief Request a switch to the next engine.
 *
 * Safe to call from another context (e.g. the button click handler). The switch
 * takes effect when the active engine next checks its stop flag.
 */
void engine_request_switch(void);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* __ENGINE_CONTROLLER_H__ */

/**
 * @}
 */

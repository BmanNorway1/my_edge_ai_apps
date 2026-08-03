/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include "engine.h"
#include "engine_controller.h"

LOG_MODULE_REGISTER(engine_ctrl);

extern const engine_t kws_engine;
#if !defined(CONFIG_KWS_ONLY)
extern const engine_t gesture_engine;
#endif

/* Active engine on boot is engines[0]. A switch request advances.
 * CONFIG_KWS_ONLY drops the gesture engine so the app never calls its init()
 * (and therefore never touches the BMI270), for development without the
 * IMU wired up.
 */
static const engine_t *const engines[] = {
	&kws_engine,
#if !defined(CONFIG_KWS_ONLY)
	&gesture_engine,
#endif
};

static size_t active_idx;
static atomic_t switch_requested;

void engine_request_switch(void)
{
	atomic_set(&switch_requested, 1);
}

int engine_controller_init(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(engines); i++) {
		int err = engines[i]->init();

		if (err) {
			LOG_ERR("Engine '%s' init failed (err %d)", engines[i]->name, err);
			return err;
		}
	}

	return 0;
}

void engine_controller_run(void)
{
	while (true) {
		atomic_clear(&switch_requested);

		const engine_t *e = engines[active_idx];

		LOG_INF("Active engine: %s", e->name);

		int err = e->enter();

		if (err) {
			LOG_ERR("Engine '%s' enter failed (err %d)", e->name, err);
		}

		/* Runs until engine_request_switch() sets the stop flag. */
		e->run(&switch_requested);

		e->exit();

		active_idx = (active_idx + 1) % ARRAY_SIZE(engines);
	}
}

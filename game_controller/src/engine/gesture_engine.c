/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * @brief Gesture inference engine: gesture recognition over the IMU.
 *
 * The IMU is brought up once in init() and left sampling for the lifetime of
 * the application; in other engines its data-ready signals are simply ignored.
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include "ble/ble_nus.h"
#include "engine.h"
#include "gesture/gesture.h"
#include "gesture/imu/imu.h"

LOG_MODULE_REGISTER(gesture_engine);

/* IMU sample rate; the imu module fires its data-ready callback at this rate. */
#define IMU_DATA_RATE_HZ 100

/* Re-check the stop flag at least this often while waiting for IMU data. */
#define IMU_SEM_TIMEOUT_MS 100

/* Signalled from the imu module's data-ready callback (timer context). */
K_SEM_DEFINE(imu_data_ready_sem, 0, 1);

/* IMU configuration the gesture model was trained with. */
static const imu_config_t imu_cfg = {
	.accel_fs_g = IMU_ACCEL_SCALE_4G,
	.gyro_fs_dps = IMU_GYRO_SCALE_1000DPS,
	.data_rate_hz = IMU_DATA_RATE_HZ,
};

/* Runs in timer context: only signal, never touch the SPI bus from here. */
static void on_imu_data_ready(void)
{
	k_sem_give(&imu_data_ready_sem);
}

static int gesture_engine_init(void)
{
	int err = gesture_init();

	if (err) {
		LOG_ERR("Gesture init failed (err %d)", err);
		return err;
	}

	status_t status = imu_init(&imu_cfg, on_imu_data_ready);

	if (status != STATUS_SUCCESS) {
		LOG_ERR("IMU init failed (status %d)", status);
		return -EIO;
	}

	return 0;
}

static int gesture_engine_enter(void)
{
	gesture_reset();
	/* Drop stale data-ready signals so the input window refills cleanly. */
	k_sem_reset(&imu_data_ready_sem);
	return 0;
}

static void gesture_engine_exit(void)
{
	/* IMU is left running; nothing to tear down. */
}

static void gesture_engine_run(atomic_t *stop)
{
	imu_data_t imu_data;
	struct gesture_prediction prediction;
	int err;

	while (!atomic_get(stop)) {
		if (k_sem_take(&imu_data_ready_sem, K_MSEC(IMU_SEM_TIMEOUT_MS)) != 0) {
			/* Timeout: loop to re-check the stop flag. */
			continue;
		}

		if (imu_read(&imu_data) != STATUS_SUCCESS) {
			LOG_ERR("IMU read failed");
			continue;
		}

		const float features[GESTURE_NUM_FEATURES] = {
			imu_data.accel[0].phys * 1000.0f,
			imu_data.accel[1].phys * 1000.0f,
			imu_data.accel[2].phys * 1000.0f,
			imu_data.gyro[0].phys * 1000.0f,
			imu_data.gyro[1].phys * 1000.0f,
			imu_data.gyro[2].phys * 1000.0f,
		};

		err = gesture_process(features, GESTURE_NUM_FEATURES, &prediction);
		if (err == -EBUSY) {
			/* Input window not full yet; keep feeding. */
			continue;
		} else if (err) {
			LOG_ERR("Gesture inference failed (err %d)", err);
			continue;
		}

		if (prediction.valid) {
			LOG_INF("Gesture: %s (class %u, prob %.2f)", prediction.name,
				prediction.target, (double)prediction.probability);

			if (prediction.command != GAME_CMD_NONE) {
				(void)ble_nus_send_command(prediction.command);
			}
		}
	}
}

const engine_t gesture_engine = {
	.name = "Gesture",
	.init = gesture_engine_init,
	.enter = gesture_engine_enter,
	.exit = gesture_engine_exit,
	.run = gesture_engine_run,
};

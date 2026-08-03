/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * @brief KWS inference engine: keyword spotting over the DMIC.
 */

#include <errno.h>
#include <stddef.h>

#include <zephyr/audio/dmic.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include "ble/ble_nus.h"
#include "engine.h"
#include "kws/dmic.h"
#include "kws/kws.h"

LOG_MODULE_REGISTER(kws_engine);

#define DMIC_READ_TIMEOUT 100

static const struct device *const dmic_dev = DEVICE_DT_GET(DT_NODELABEL(dmic_dev));

static int kws_engine_init(void)
{
	/* The DMIC is configured per session in enter()/exit(); only the model
	 * needs one-time setup here.
	 */
	int err = kws_init();

	if (err) {
		LOG_ERR("KWS init failed (err %d)", err);
		return err;
	}

	return 0;
}

static int kws_engine_enter(void)
{
	int err = dmic_init();

	if (err) {
		LOG_ERR("DMIC configure failed (err %d)", err);
		return err;
	}

	kws_reset();

	err = dmic_trigger(dmic_dev, DMIC_TRIGGER_START);
	if (err < 0) {
		LOG_ERR("Failed to start DMIC (err %d)", err);
		return err;
	}

	return 0;
}

/* Bound on how long to wait for the asynchronous stop to complete. */
#define DMIC_STOP_SETTLE_MS 100
#define DMIC_STOP_POLL_MS   5

static void kws_engine_exit(void)
{
	int err = dmic_trigger(dmic_dev, DMIC_TRIGGER_STOP);

	if (err < 0) {
		LOG_ERR("Failed to stop DMIC (err %d)", err);
	}

	/* Reclaim buffers the driver queued for reading: the driver never purges
	 * its RX queue, so blocks left there stay allocated in the slab across the
	 * uninit. Then fully uninitialize the PDM so it cannot touch slab memory
	 * while the gesture engine is active. The stop is asynchronous, so
	 * dmic_deinit() returns -EBUSY until the stream goes inactive; retry until
	 * it takes.
	 */
	void *buffer;
	size_t size;

	for (int waited = 0; waited <= DMIC_STOP_SETTLE_MS; waited += DMIC_STOP_POLL_MS) {
		while (dmic_read(dmic_dev, 0, &buffer, &size, 0) == 0) {
			free_dmic_buffer(buffer);
		}

		err = dmic_deinit();
		if (err != -EBUSY) {
			break;
		}

		k_msleep(DMIC_STOP_POLL_MS);
	}

	if (err) {
		LOG_ERR("Failed to deinit DMIC (err %d)", err);
	}
}

static void kws_engine_run(atomic_t *stop)
{
	void *audio_buffer;
	size_t audio_buffer_size;
	struct kws_prediction prediction;
	int err;

	while (!atomic_get(stop)) {
		err = dmic_read(dmic_dev, 0, &audio_buffer, &audio_buffer_size,
				DMIC_READ_TIMEOUT);
		if (err < 0) {
			LOG_ERR("Failed to read from DMIC (err %d)", err);
			return;
		}

		/* kws_process takes ownership of audio_buffer and frees it. */
		err = kws_process(audio_buffer, DMIC_SAMPLES_IN_BLOCK, &prediction);
		if (err == -EBUSY) {
			/* Model needs more data; keep streaming. */
			continue;
		} else if (err) {
			LOG_ERR("Keyword spotting failed (err %d)", err);
			return;
		}

		if (prediction.valid) {
			//LOG_INF("Keyword: %s (class %u, prob %.2f)", prediction.name,
			//						prediction.class, (double)prediction.avg_probability);

			/* Low-latency control output for the serial game controller,
			 * matching the ww_kws "Keyword spotted: X" format. printk is
			 * synchronous (CONFIG_LOG_PRINTK=n) so it is not delayed by the
			 * deferred log thread.
			 */
			printk("Keyword spotted: %s\r\n", prediction.name);

			if (prediction.command != GAME_CMD_NONE) {
				(void)ble_nus_send_command(prediction.command);
			}
		}
	}
}

const engine_t kws_engine = {
	.name = "KWS",
	.init = kws_engine_init,
	.enter = kws_engine_enter,
	.exit = kws_engine_exit,
	.run = kws_engine_run,
};

/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <stddef.h>

#include <zephyr/audio/dmic.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "dmic.h"

LOG_MODULE_REGISTER(dmic);

#define BLOCK_SIZE (DMIC_SAMPLE_BYTES * DMIC_SAMPLES_IN_BLOCK)

K_MEM_SLAB_DEFINE_STATIC(dmic_mem_slab, BLOCK_SIZE, 4, 4);

int dmic_init(void)
{
	int err;
	const struct device *const dmic_dev = DEVICE_DT_GET(DT_NODELABEL(dmic_dev));

	if (!device_is_ready(dmic_dev)) {
		LOG_ERR("Device is not ready");
		return -ENODEV;
	}

	struct pcm_stream_cfg stream = {
		.pcm_rate = DMIC_PCM_RATE,
		.pcm_width = DMIC_SAMPLE_BYTES * 8,
		.block_size = BLOCK_SIZE,
		.mem_slab = &dmic_mem_slab,
	};
	struct dmic_cfg cfg = {
		.io = {
			.min_pdm_clk_freq = 1000000,
			.max_pdm_clk_freq = 3250000,
			.min_pdm_clk_dc = 40,
			.max_pdm_clk_dc = 60,
		},
		.streams = &stream,
		.channel = {
			.req_chan_map_lo = dmic_build_channel_map(0, 0, PDM_CHAN_LEFT),
			.req_chan_map_hi = 0,
			.req_num_chan = 1,
			.req_num_streams = 1,
		},
	};

	err = dmic_configure(dmic_dev, &cfg);
	if (err < 0) {
		LOG_ERR("Failed to configure (err %d)", err);
		return err;
	}

	return 0;
}

int dmic_deinit(void)
{
	const struct device *const dmic_dev = DEVICE_DT_GET(DT_NODELABEL(dmic_dev));

	/* A zero pcm_rate makes the driver fully uninitialize the PDM peripheral
	 * (nrfx_pdm_uninit), which halts DMA outright. A soft DMIC_TRIGGER_STOP
	 * leaves the peripheral live and it keeps writing into buffers that have
	 * been returned to the slab, corrupting the slab free list.
	 * Channel config must still be valid: it is checked before the rate==0
	 * early-out in the driver's configure path.
	 */
	struct pcm_stream_cfg stream = {
		.pcm_rate = 0,
		.pcm_width = 0,
		.block_size = BLOCK_SIZE,
		.mem_slab = &dmic_mem_slab,
	};
	struct dmic_cfg cfg = {
		.streams = &stream,
		.channel = {
			.req_chan_map_lo = dmic_build_channel_map(0, 0, PDM_CHAN_LEFT),
			.req_chan_map_hi = 0,
			.req_num_chan = 1,
			.req_num_streams = 1,
		},
	};

	/* Returns -EBUSY until the stream is inactive; the caller retries, so do
	 * not log that case here.
	 */
	return dmic_configure(dmic_dev, &cfg);
}

void free_dmic_buffer(void *buffer)
{
	k_mem_slab_free(&dmic_mem_slab, buffer);
}

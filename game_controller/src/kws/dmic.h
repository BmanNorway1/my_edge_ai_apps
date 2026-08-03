/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/**
 * @defgroup dmic DMIC control functions
 * @{
 * @ingroup ww_kws
 */

#ifndef __DMIC_H__
#define __DMIC_H__

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#define SAMPLES_BLOCK_LENGTH_MS (10)
#define DMIC_SAMPLE_BYTES	(2)
#define DMIC_PCM_RATE		(16000)
#define DMIC_SAMPLES_IN_BLOCK	(DMIC_PCM_RATE * SAMPLES_BLOCK_LENGTH_MS / 1000)

/**
 * @brief Initialize (configure) the DMIC.
 *
 * @return Operation status result, 0 for success.
 */
int dmic_init(void);

/**
 * @brief Uninitialize the DMIC, fully halting the PDM peripheral.
 *
 * Use this instead of relying on @c DMIC_TRIGGER_STOP when the DMIC will be
 * idle for a while; a soft stop leaves the peripheral running.
 *
 * @return Operation status result, 0 for success.
 */
int dmic_deinit(void);

/**
 * @brief Free the audio buffer acquired with @c dmic_read.
 *
 * @param buffer Audio buffer.
 */
void free_dmic_buffer(void *buffer);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* __DMIC_H__ */

/**
 * @}
 */

/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <errno.h>

#include <zephyr/logging/log.h>

#include <nrf_edgeai/nrf_edgeai.h>
#include <nrf_edgeai/rt/nrf_edgeai_runtime_aux.h>

#include "gesture.h"
#include "inference_postprocessing.h"
#include "nrf_edgeai_generated/nrf54lm20dk/Axon/nrf_edgeai_user_model.h"

LOG_MODULE_REGISTER(gesture);

static nrf_edgeai_t *gesture_model;

int gesture_init(void)
{
	gesture_model = nrf_edgeai_user_model_36038();
	__ASSERT_NO_MSG(gesture_model);

	nrf_edgeai_err_t err = nrf_edgeai_init(gesture_model);

	if (err) {
		LOG_ERR("Model initialization failed (err %d)", err);
		return -ENOENT;
	}

	return 0;
}

int gesture_process(const float *const features, const uint16_t num_features,
		    struct gesture_prediction *const prediction)
{
	__ASSERT_NO_MSG(features);
	__ASSERT_NO_MSG(num_features == GESTURE_NUM_FEATURES);
	__ASSERT_NO_MSG(prediction);

	prediction->valid = false;
	prediction->command = GAME_CMD_NONE;

	nrf_edgeai_err_t err;

	err = nrf_edgeai_feed_inputs(gesture_model, (void *)features, num_features);
	if (err == NRF_EDGEAI_ERR_INPROGRESS) {
		/* Input window not full yet; keep feeding. */
		return -EBUSY;
	} else if (err) {
		LOG_ERR("Failed to feed inputs (err %d)", err);
		return -EPERM;
	}

	err = nrf_edgeai_run_inference(gesture_model);
	if (err == NRF_EDGEAI_ERR_INPROGRESS) {
		/* Not enough data to extract outputs yet. */
		return -EBUSY;
	} else if (err) {
		LOG_ERR("Failed to run inference (err %d)", err);
		return -EPERM;
	}

	const uint16_t predicted = gesture_model->decoded_output.classif.predicted_class;
	const float probability =
		gesture_model->decoded_output.classif.probabilities.p_f32[predicted];

	const prediction_ctx_t result = inference_postprocess(predicted, probability);

	/* Only surface deliberate gestures; IDLE/UNKNOWN are filtered out. */
	if (result.target > CLASS_LABEL_UNKNOWN) {
		prediction->valid = true;
		prediction->target = result.target;
		prediction->probability = result.probability;
		prediction->name = inference_get_class_name((class_label_t)result.target);

		/* Only swipe gestures map to directional commands. */
		switch (result.target) {
		case CLASS_LABEL_SWIPE_LEFT:
			prediction->command = GAME_CMD_LEFT;
			break;
		case CLASS_LABEL_SWIPE_RIGHT:
			prediction->command = GAME_CMD_RIGHT;
			break;
		default:
			break;
		}
	}

	return 0;
}

void gesture_reset(void)
{
	nrf_edgeai_model_axon_init_persistent_vars(gesture_model);
}

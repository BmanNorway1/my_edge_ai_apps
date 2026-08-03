/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/**
 * @defgroup gesture Gesture recognition model functions
 * @{
 * @ingroup game_controller
 */

#ifndef __GESTURE_H__
#define __GESTURE_H__

#include <stdbool.h>
#include <stdint.h>

#include "ble/game_command.h"

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/** Number of input features fed per IMU sample (3 accel + 3 gyro). */
#define GESTURE_NUM_FEATURES 6

/**
 * @brief Prediction from the gesture recognition model.
 */
struct gesture_prediction {
	/** Prediction valid flag (a recognized, non-idle gesture). */
	bool valid;
	/** Postprocessed class label (@ref class_label_t). */
	uint16_t target;
	/** Prediction probability. */
	float probability;
	/** Class name, or NULL for invalid labels. */
	const char *name;
	/** Directional command for this gesture, or @ref GAME_CMD_NONE. */
	enum game_command command;
};

/**
 * @brief Initialize the gesture recognition model.
 *
 * @return Operation status, 0 for success.
 */
int gesture_init(void);

/**
 * @brief Feed one IMU sample to the gesture model and run inference when the
 *        input window is full.
 *
 * @param features      Array of @p num_features floats (accel x/y/z, gyro x/y/z).
 * @param num_features  Number of features, must be @ref GESTURE_NUM_FEATURES.
 * @param[out] prediction Result of gesture recognition.
 *
 * @retval 0 Operation successful (inference ran; check @c prediction->valid).
 * @retval -EBUSY Model needs more data (input window not full yet).
 * @retval -EPERM Operation failed on nRF Edge AI Lib level.
 */
int gesture_process(const float *features, uint16_t num_features,
		    struct gesture_prediction *prediction);

/**
 * @brief Reset the gesture model state.
 */
void gesture_reset(void);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* __GESTURE_H__ */

/**
 * @}
 */

/*
 * Copyright (c) 2026 Nordic Semiconductor ASA
 *
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

/**
 * @defgroup leds LEDs control functions
 * @{
 * @ingroup ww_kws
 */

#ifndef __LEDS_H__
#define __LEDS_H__

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

/**
 * @brief Initialize LEDs.
 *
 * @return Operation status, 0 for success.
 */
int leds_init(void);

/**
 * @brief Blink with LED0.
 */
void leds_blink_led0(void);

/**
 * @brief Blink with LED1.
 */
void leds_blink_led1(void);

/**
 * @brief Turn on LED0.
 */
void leds_on_led0(void);

/**
 * @brief Turn off LED0.
 */
void leds_off_led0(void);

/** Number of controllable LEDs. */
#define LEDS_COUNT 4

/**
 * @brief Light only the LED at the given index, turning all others off.
 *
 * @param idx Index of the LED to light, from 0 to @c LEDS_COUNT - 1.
 */
void leds_set_only(unsigned int idx);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* __LEDS_H__ */

/**
 * @}
 */

/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 *
 * Nordic UART Service (NUS) sender interface.
 *
 * The whole module compiles out when Bluetooth is disabled in prj.conf:
 * src/ble_nus.c is dropped from the build (see CMakeLists.txt) and the calls
 * below collapse into no-op stubs. Callers therefore need no #ifdefs of their
 * own -- main.c builds unchanged either way.
 */

#ifndef __BLE_NUS_H__
#define __BLE_NUS_H__

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <errno.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_BT

int init_ble_nus(const struct gpio_dt_spec *led);
bool ble_nus_ready(void);
int ble_nus_send(const void *data, uint16_t len);

#else /* !CONFIG_BT -- BLE disabled, stub the interface out */

/* Signatures must track the CONFIG_BT declarations above exactly: main.c is
 * compiled against whichever branch is active, so a stub that drifts only breaks
 * the build in the configuration nobody happens to be building.
 */
static inline int init_ble_nus(const struct gpio_dt_spec *led)
{
	ARG_UNUSED(led);
	return 0;
}

static inline bool ble_nus_ready(void)
{
	return false;
}

static inline int ble_nus_send(const void *data, uint16_t len)
{
	ARG_UNUSED(data);
	ARG_UNUSED(len);
	return -ENOTSUP;
}

#endif /* CONFIG_BT */

#ifdef __cplusplus
}
#endif

#endif /* __BLE_NUS_H__ */

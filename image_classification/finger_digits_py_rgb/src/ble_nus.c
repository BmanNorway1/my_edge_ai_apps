/*
 * Copyright (c) 2026
 * SPDX-License-Identifier: LicenseRef-Nordic-5-Clause
 */

#include <zephyr/kernel.h>
#include <errno.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/drivers/gpio.h>

#include <bluetooth/services/nus.h>

#include <zephyr/logging/log.h>

#include "ble_nus.h"

LOG_MODULE_REGISTER(ble_nus, LOG_LEVEL_INF);

#define DEVICE_NAME     CONFIG_BT_DEVICE_NAME
#define DEVICE_NAME_LEN (sizeof(DEVICE_NAME) - 1)

static struct bt_conn *current_conn;
static volatile bool notify_enabled;
static const struct gpio_dt_spec *conn_led;

static struct k_work adv_work;

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, DEVICE_NAME, DEVICE_NAME_LEN),
};

static const struct bt_data sd[] = {
	BT_DATA_BYTES(BT_DATA_UUID128_ALL, BT_UUID_NUS_VAL),
};

static void adv_work_handler(struct k_work *work)
{
	int err = bt_le_adv_start(BT_LE_ADV_CONN_FAST_2, ad, ARRAY_SIZE(ad),
				   sd, ARRAY_SIZE(sd));
	if (err) {
		LOG_ERR("Advertising failed to start (err %d)", err);
		return;
	}

	LOG_INF("Advertising started as \"%s\"", DEVICE_NAME);
}

static void advertising_start(void)
{
	k_work_submit(&adv_work);
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	char addr[BT_ADDR_LE_STR_LEN];

	if (err) {
		LOG_ERR("Connection failed (err 0x%02x)", err);
		return;
	}

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("Connected %s", addr);

	current_conn = bt_conn_ref(conn);
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	char addr[BT_ADDR_LE_STR_LEN];

	bt_addr_le_to_str(bt_conn_get_dst(conn), addr, sizeof(addr));
	LOG_INF("Disconnected: %s (reason 0x%02x)", addr, reason);

	notify_enabled = false;

	if (current_conn) {
		bt_conn_unref(current_conn);
		current_conn = NULL;
	}

	if (conn_led) {
		gpio_pin_set_dt(conn_led, 0);
	}
}

static void recycled_cb(void)
{
	advertising_start();
}

BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected    = connected,
	.disconnected = disconnected,
	.recycled     = recycled_cb,
};

static void nus_send_enabled_cb(enum bt_nus_send_status status)
{
	notify_enabled = (status == BT_NUS_SEND_STATUS_ENABLED);
	LOG_INF("NUS notifications %s", notify_enabled ? "enabled" : "disabled");

	if (conn_led) {
		gpio_pin_set_dt(conn_led, notify_enabled ? 1 : 0);
	}
}

static struct bt_nus_cb nus_cb = {
	.send_enabled = nus_send_enabled_cb,
};

int init_ble_nus(const struct gpio_dt_spec *led)
{
	int err;

	conn_led = led;

	k_work_init(&adv_work, adv_work_handler);

	err = bt_enable(NULL);
	if (err) {
		LOG_ERR("bt_enable failed (err %d)", err);
		return err;
	}
	LOG_INF("Bluetooth initialized");

	err = bt_nus_init(&nus_cb);
	if (err) {
		LOG_ERR("bt_nus_init failed (err %d)", err);
		return err;
	}

	advertising_start();
	return 0;
}

bool ble_nus_ready(void)
{
	return (current_conn != NULL) && notify_enabled;
}

int ble_nus_send(const void *data, uint16_t len)
{
	if (!ble_nus_ready()) {
		return -ENOTCONN;
	}

	return bt_nus_send(current_conn, (const uint8_t *)data, len);
}
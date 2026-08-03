#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>

#include "dispenser.h"

#define DISPENSE_TIME K_SECONDS(2)

static const struct gpio_dt_spec dispenser = GPIO_DT_SPEC_GET(DT_ALIAS(ww_out), gpios);

static void dispense_timer_expiry(struct k_timer *timer);
static K_TIMER_DEFINE(dispense_timer, dispense_timer_expiry, NULL);

int dispenser_init(void)
{
	if (!gpio_is_ready_dt(&dispenser)) {
        return -ENODEV;
    }
    gpio_pin_configure_dt(&dispenser, GPIO_OUTPUT_INACTIVE);
    return 0;
}

void dispenser_dispense(void)
{
    gpio_pin_set_dt(&dispenser, 1);
    k_timer_user_data_set(&dispense_timer, (void *)&dispenser);
    k_timer_start(&dispense_timer, DISPENSE_TIME, K_NO_WAIT);
}

static void dispense_timer_expiry(struct k_timer *timer)
{
    const struct gpio_dt_spec *dispenser = k_timer_user_data_get(timer);
    gpio_pin_set_dt(dispenser, 0);
}
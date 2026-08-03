#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>

#include "button.h"
#include "dispenser.h"
#include "leds.h"

static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(DT_ALIAS(my_button), gpios);

static struct gpio_callback button_cb_data;

int button_init(void)
{
    if (!gpio_is_ready_dt(&button)) {
        return -ENODEV;
    }

    int err = gpio_pin_configure_dt(&button, GPIO_INPUT | GPIO_PULL_UP);
    if (err) {
        return err;
    }

    return gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE);
}

void button_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    dispenser_dispense();
    leds_blink_led1();
}

void button_setup_interrupt(void)
{
    gpio_init_callback(&button_cb_data, button_pressed, BIT(button.pin));
    gpio_add_callback(button.port, &button_cb_data);
}
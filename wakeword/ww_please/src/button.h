#ifndef __BUTTON_H__
#define __BUTTON_H__

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

int button_init(void);

void button_pressed(const struct device *dev, struct gpio_callback *cb, uint32_t pins);

void button_setup_interrupt(void);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* __BUTTON_H__ */
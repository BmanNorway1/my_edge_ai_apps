#include <zephyr/kernel.h>

int init_ble_nus(void);
bool ble_nus_ready(void);
int ble_nus_send(const void *data, uint16_t len);
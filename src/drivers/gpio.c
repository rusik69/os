/*
 * gpio.c — GPIO framework driver
 *
 * Simple GPIO framework using port-based I/O.
 * Provides pin request/set/get/free operations.
 */

#include "gpio.h"
#include "io.h"
#include "printf.h"
#include "string.h"

static int g_gpio_init_done = 0;
static struct gpio_pin g_gpio_pins[GPIO_MAX_PINS];

int gpio_init(void) {
    if (g_gpio_init_done)
        return 0;

    memset(g_gpio_pins, 0, sizeof(g_gpio_pins));

    g_gpio_init_done = 1;
    kprintf("[GPIO] Framework initialized (%d pins max)\n", GPIO_MAX_PINS);
    return 0;
}

int gpio_request(int pin, int direction) {
    if (!g_gpio_init_done)
        return -1;
    if (pin < 0 || pin >= GPIO_MAX_PINS)
        return -1;
    if (g_gpio_pins[pin].used)
        return -1;

    g_gpio_pins[pin].used = 1;
    g_gpio_pins[pin].pin = pin;
    g_gpio_pins[pin].direction = direction;

    return 0;
}

void gpio_set_value(int pin, int value) {
    if (!g_gpio_init_done || pin < 0 || pin >= GPIO_MAX_PINS)
        return;
    if (!g_gpio_pins[pin].used || g_gpio_pins[pin].direction != GPIO_DIR_OUT)
        return;

    /* Use port-based GPIO via 0xE port (simulated) */
    uint8_t port_val = inb(GPIO_BASE_PORT);
    if (value)
        port_val |= (1 << (pin & 7));
    else
        port_val &= ~(1 << (pin & 7));
    outb(GPIO_BASE_PORT, port_val);
}

int gpio_get_value(int pin) {
    if (!g_gpio_init_done || pin < 0 || pin >= GPIO_MAX_PINS)
        return -1;
    if (!g_gpio_pins[pin].used)
        return -1;

    uint8_t port_val = inb(GPIO_BASE_PORT);
    return (port_val >> (pin & 7)) & 1;
}

void gpio_free(int pin) {
    if (!g_gpio_init_done || pin < 0 || pin >= GPIO_MAX_PINS)
        return;

    g_gpio_pins[pin].used = 0;
    g_gpio_pins[pin].direction = 0;
    g_gpio_pins[pin].pin = 0;
}

int gpio_is_initialized(void) {
    return g_gpio_init_done;
}

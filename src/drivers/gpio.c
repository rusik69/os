/*
 * gpio.c — GPIO driver using port-mapped I/O
 *
 * Provides a simple GPIO API for manipulating pins via I/O ports.
 * Supports up to GPIO_MAX_PINS (64) pins, with direction control
 * and value get/set operations.  Uses a fixed port-mapped register
 * layout where each pin's data/control is accessed through a
 * GPIO base port.
 *
 * This implementation targets the standard PCH GPIO on Intel chipsets
 * (port 0xE for data, port 0xF for direction), but the API is generic
 * enough to support other layouts by modifying the register offsets.
 */

#include "gpio.h"
#include "io.h"
#include "printf.h"
#include "string.h"
#include "errno.h"

/* ── GPIO register layout ─────────────────────────────────────────── */

/* Data register: each bit represents one GPIO pin's value (0/1) */
#define GPIO_DATA_PORT      0xE

/* Direction register: 0 = input, 1 = output */
#define GPIO_DIR_PORT       0xF

/* ── Static state ─────────────────────────────────────────────────── */

static struct gpio_pin g_pins[GPIO_MAX_PINS];
static int g_gpio_initialised = 0;

/* ── Public API ───────────────────────────────────────────────────── */

int gpio_init(void)
{
    memset(g_pins, 0, sizeof(g_pins));
    g_gpio_initialised = 1;
    kprintf("[GPIO] Initialised (%d pins max)\n", GPIO_MAX_PINS);
    return 0;
}

int gpio_request(int pin, int direction)
{
    if (!g_gpio_initialised) return -EIO;
    if (pin < 0 || pin >= GPIO_MAX_PINS) return -EINVAL;
    if (g_pins[pin].used) return -EBUSY;

    g_pins[pin].used = 1;
    g_pins[pin].pin = pin;
    g_pins[pin].direction = direction;

    /* Set direction in the hardware register */
    uint8_t dir = inb(GPIO_DIR_PORT);
    if (direction == GPIO_DIR_OUT)
        dir |= (1u << (pin % 8));
    else
        dir &= ~(1u << (pin % 8));
    outb(GPIO_DIR_PORT, dir);

    return 0;
}

void gpio_set_value(int pin, int value)
{
    if (!g_gpio_initialised || pin < 0 || pin >= GPIO_MAX_PINS)
        return;
    if (!g_pins[pin].used || g_pins[pin].direction != GPIO_DIR_OUT)
        return;

    uint8_t data = inb(GPIO_DATA_PORT);
    if (value)
        data |= (1u << (pin % 8));
    else
        data &= ~(1u << (pin % 8));
    outb(GPIO_DATA_PORT, data);
}

int gpio_get_value(int pin)
{
    if (!g_gpio_initialised || pin < 0 || pin >= GPIO_MAX_PINS)
        return 0;
    if (!g_pins[pin].used)
        return 0;

    uint8_t data = inb(GPIO_DATA_PORT);
    return (data >> (pin % 8)) & 1;
}

void gpio_free(int pin)
{
    if (!g_gpio_initialised || pin < 0 || pin >= GPIO_MAX_PINS)
        return;
    g_pins[pin].used = 0;
    g_pins[pin].direction = 0;

    /* Set as input (safe default) */
    uint8_t dir = inb(GPIO_DIR_PORT);
    dir &= ~(1u << (pin % 8));
    outb(GPIO_DIR_PORT, dir);
}

int gpio_is_initialized(void)
{
    return g_gpio_initialised;
}

void gpio_set_irq_mode(unsigned int pin, int mode)
{
    (void)pin;
    (void)mode;
    /* IRQ mode configuration not implemented for port-based GPIO */
}

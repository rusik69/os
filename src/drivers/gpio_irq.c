/*
 * gpio_irq.c — GPIO interrupt support (B13)
 *
 * Extends the existing gpio.c infrastructure with IRQ capabilities:
 * - gpio_to_irq() converts GPIO pin number to IRQ number
 * - GPIO pins can be configured as interrupt sources (edge/level)
 * - Interrupt handlers registered via request_irq()
 *
 * Uses the existing irq_chip abstraction for integration.
 */

#include "gpio.h"
#include "idt.h"
#include "io.h"
#include "printf.h"
#include "heap.h"
#include "string.h"
#include "errno.h"

/* Per-pin interrupt config */
struct gpio_irq_pin {
    int  in_use;
    int  pin;
    int  irq_num;
    int  mode;          /* 0=disabled, 1=rising, 2=falling, 3=both, 4=low, 5=high */
    void (*handler)(void *arg);
    void *arg;
};

/* GPIO IRQ controller state */
#define MAX_GPIO_IRQ_PINS  16
static struct gpio_irq_pin g_gpio_irqs[MAX_GPIO_IRQ_PINS];
static int g_gpio_irq_base = 64;  /* IRQ base number for GPIO */
static int g_gpio_irq_initialized = 0;

/*
 * gpio_irq_init — initialize GPIO interrupt subsystem
 */
static int gpio_irq_init(void)
{
    if (g_gpio_irq_initialized) return 0;

    memset(g_gpio_irqs, 0, sizeof(g_gpio_irqs));

    /* Reserve IRQ numbers from base */
    g_gpio_irq_base = irq_alloc_range(MAX_GPIO_IRQ_PINS);
    if (g_gpio_irq_base < 0) {
        kprintf("[GPIO-IRQ] Failed to allocate IRQ range\n");
        return g_gpio_irq_base;
    }

    g_gpio_irq_initialized = 1;
    kprintf("[GPIO-IRQ] Initialized, IRQ base=%d, %d pins\n",
            g_gpio_irq_base, MAX_GPIO_IRQ_PINS);
    return 0;
}

/*
 * gpio_irq_alloc — allocate an IRQ number for a GPIO pin
 *
 * Returns IRQ number that can be used with the GPIO IRQ subsystem, or <0 on error.
 */
static int gpio_irq_alloc(unsigned int gpio_pin)
{
    if (!g_gpio_irq_initialized) return -EAGAIN;

    /* Find or allocate an IRQ slot for this GPIO pin */
    for (int i = 0; i < MAX_GPIO_IRQ_PINS; i++) {
        if (g_gpio_irqs[i].in_use && g_gpio_irqs[i].pin == (int)gpio_pin) {
            return g_gpio_irqs[i].irq_num;
        }
    }

    /* Allocate new slot */
    for (int i = 0; i < MAX_GPIO_IRQ_PINS; i++) {
        if (!g_gpio_irqs[i].in_use) {
            g_gpio_irqs[i].in_use = 1;
            g_gpio_irqs[i].pin = (int)gpio_pin;
            g_gpio_irqs[i].irq_num = g_gpio_irq_base + i;
            g_gpio_irqs[i].mode = 0;
            g_gpio_irqs[i].handler = NULL;
            g_gpio_irqs[i].arg = NULL;
            return g_gpio_irqs[i].irq_num;
        }
    }

    return -ENOSPC;
}

/*
 * gpio_irq_set_mode — configure interrupt trigger mode for GPIO pin
 *
 * @gpio_pin: GPIO pin number
 * @mode: 1=rising edge, 2=falling edge, 3=both edges, 4=low level, 5=high level
 */
static int gpio_irq_set_mode(unsigned int gpio_pin, int mode)
{
    if (!g_gpio_irq_initialized) return -EAGAIN;
    if (mode < 1 || mode > 5) return -EINVAL;

    for (int i = 0; i < MAX_GPIO_IRQ_PINS; i++) {
        if (g_gpio_irqs[i].in_use && g_gpio_irqs[i].pin == (int)gpio_pin) {
            g_gpio_irqs[i].mode = mode;

            /* Configure the actual GPIO hardware for interrupt */
            gpio_set_irq_mode(gpio_pin, mode);
            return 0;
        }
    }

    return -ENOENT;
}

/*
 * gpio_irq_handler — internal handler called from GPIO controller IRQ
 *
 * Called by the GPIO chip's ISR when a GPIO pin triggers an interrupt.
 * Dispatches to the registered handler.
 */
static void gpio_irq_dispatch(int pin)
{
    for (int i = 0; i < MAX_GPIO_IRQ_PINS; i++) {
        if (g_gpio_irqs[i].in_use && g_gpio_irqs[i].pin == pin) {
            if (g_gpio_irqs[i].handler) {
                g_gpio_irqs[i].handler(g_gpio_irqs[i].arg);
            }
            return;
        }
    }
}

/*
 * GPIO IRQ handler registration — called from request_irq path
 */
static int gpio_irq_register(int irq_num, void (*handler)(void *), void *arg)
{
    for (int i = 0; i < MAX_GPIO_IRQ_PINS; i++) {
        if (g_gpio_irqs[i].in_use && g_gpio_irqs[i].irq_num == irq_num) {
            g_gpio_irqs[i].handler = handler;
            g_gpio_irqs[i].arg = arg;
            return 0;
        }
    }
    return -ENOENT;
}

/*
 * gpio_irq_unregister — unregister GPIO IRQ handler
 */
static int gpio_irq_unregister(int irq_num)
{
    for (int i = 0; i < MAX_GPIO_IRQ_PINS; i++) {
        if (g_gpio_irqs[i].in_use && g_gpio_irqs[i].irq_num == irq_num) {
            g_gpio_irqs[i].handler = NULL;
            g_gpio_irqs[i].arg = NULL;
            g_gpio_irqs[i].mode = 0;
            return 0;
        }
    }
    return -ENOENT;
}
#include "module.h"
module_init(gpio_irq_init);

/* ── gpio_irq_enable: Enable GPIO interrupt for a pin ──────── */
static int gpio_irq_enable(int gpio)
{
    if (gpio < 0 || gpio >= GPIO_MAX_PINS) return -EINVAL;

    /* Configure the GPIO pin for interrupt mode */
    gpio_set_irq_mode((unsigned int)gpio, 1); /* rising edge */

    kprintf("[gpio] GPIO IRQ enabled for pin %d\n", gpio);
    return 0;
}

/* ── gpio_irq_disable: Disable GPIO interrupt for a pin ──────── */
static int gpio_irq_disable(int gpio)
{
    if (gpio < 0 || gpio >= GPIO_MAX_PINS) return -EINVAL;

    /* Disable interrupt mode for this pin */
    gpio_set_irq_mode((unsigned int)gpio, 0); /* disabled */

    kprintf("[gpio] GPIO IRQ disabled for pin %d\n", gpio);
    return 0;
}

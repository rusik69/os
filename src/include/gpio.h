#ifndef GPIO_H
#define GPIO_H

#include "types.h"

/* GPIO directions */
#define GPIO_DIR_IN  0
#define GPIO_DIR_OUT 1

/* GPIO value */
#define GPIO_LOW  0
#define GPIO_HIGH 1

/* Maximum number of GPIO pins (port 0xE/0xF are 8-bit ports) */
#define GPIO_MAX_PINS 8

/* GPIO pin descriptor */
struct gpio_pin {
    int      used;
    int      direction;  /* GPIO_DIR_IN or GPIO_DIR_OUT */
    int      pin;        /* Pin number (0..63) */
};

/* GPIO API */
int  gpio_init(void);
int  gpio_request(int pin, int direction);
void gpio_set_value(int pin, int value);
int  gpio_get_value(int pin);
void gpio_free(int pin);
int  gpio_is_initialized(void);

/* GPIO IRQ mode configuration (used by gpio_irq.c) */
void gpio_set_irq_mode(unsigned int pin, int mode);

/* Simple GPIO using port 0xE (PCH GPIO on some Intel chipsets) */
#define GPIO_BASE_PORT 0xE

#endif /* GPIO_H */

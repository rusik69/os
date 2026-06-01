#ifndef GPIOLIB_H
#define GPIOLIB_H

#include "types.h"

/*
 * GPIO library abstraction.
 *
 * Provides a simple GPIO control interface abstracting over
 * x86-64 I/O ports (0x60/0x61 for keyboard port, 0x80 for
 * POST codes, etc.) and memory-mapped GPIO on virtual platforms.
 *
 * Based on concepts from Linux gpiolib.
 */

#define GPIO_NAME_MAX           32
#define GPIO_MAX_CHIPS          8
#define GPIO_MAX_LINES          32

/* GPIO direction */
#define GPIO_DIR_IN             0
#define GPIO_DIR_OUT            1

/* GPIO value */
#define GPIO_LOW                0
#define GPIO_HIGH               1

/* GPIO flags */
#define GPIO_ACTIVE_LOW         (1 << 0)
#define GPIO_OPEN_DRAIN         (1 << 1)
#define GPIO_OPEN_SOURCE        (1 << 2)

struct gpio_chip;

/* GPIO chip operations */
struct gpio_chip_ops {
    int  (*request)(struct gpio_chip *chip, unsigned int offset);
    void (*free)(struct gpio_chip *chip, unsigned int offset);
    int  (*direction_input)(struct gpio_chip *chip, unsigned int offset);
    int  (*direction_output)(struct gpio_chip *chip, unsigned int offset,
                             int value);
    int  (*get)(struct gpio_chip *chip, unsigned int offset);
    void (*set)(struct gpio_chip *chip, unsigned int offset, int value);
};

struct gpio_chip {
    const char            *label;
    struct gpio_chip_ops  *ops;
    unsigned int           base;       /* global GPIO number base */
    unsigned int           ngpio;      /* number of GPIO lines */
    uint32_t               valid_mask; /* bitmask of valid lines */
};

/*
 * gpio_request  - Request a GPIO line for exclusive use.
 * Returns 0 on success, negative on error.
 */
int gpio_request(unsigned int gpio, const char *label);

/*
 * gpio_free  - Release a previously requested GPIO line.
 */
void gpio_free(unsigned int gpio);

/*
 * gpio_direction_input  - Set a GPIO line as input.
 */
int gpio_direction_input(unsigned int gpio);

/*
 * gpio_direction_output  - Set a GPIO line as output with initial value.
 */
int gpio_direction_output(unsigned int gpio, int value);

/*
 * gpio_get_value  - Read the current value of a GPIO line.
 */
int gpio_get_value(unsigned int gpio);

/*
 * gpio_set_value  - Set the value of a GPIO output line.
 */
void gpio_set_value(unsigned int gpio, int value);

/*
 * gpiochip_add  - Register a GPIO chip with the library.
 */
int gpiochip_add(struct gpio_chip *chip);

/*
 * gpiolib_init  - Initialise the GPIO library.
 */
void gpiolib_init(void);

#endif /* GPIOLIB_H */

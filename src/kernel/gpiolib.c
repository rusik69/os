#include "gpiolib.h"
#include "printf.h"
#include "kernel.h"
#include "string.h"
#include "spinlock.h"

/*
 * GPIO library — chip-based abstraction over x86-64 I/O ports.
 *
 * Chips register with gpiochip_add(); the library maps global GPIO
 * numbers to chip+offset and dispatches through the chip's ops.
 */

#define GPIO_MAX_GPIOS (GPIO_MAX_CHIPS * GPIO_MAX_LINES)

static struct gpio_chip *gpio_chips[GPIO_MAX_CHIPS];
static int gpio_chip_count;
static spinlock_t gpiolib_lock;

/* Per-GPIO allocation tracking */
static char gpio_labels[GPIO_MAX_GPIOS][GPIO_NAME_MAX];
static int  gpio_allocated[GPIO_MAX_GPIOS];

static int gpio_to_chip_offset(unsigned int gpio,
                               struct gpio_chip **chip,
                               unsigned int *offset)
{
    for (int i = 0; i < gpio_chip_count; i++) {
        if (!gpio_chips[i])
            continue;
        if (gpio >= gpio_chips[i]->base &&
            gpio < gpio_chips[i]->base + gpio_chips[i]->ngpio) {
            *chip = gpio_chips[i];
            *offset = gpio - gpio_chips[i]->base;
            return 0;
        }
    }
    return -1;
}

int gpio_request(unsigned int gpio, const char *label)
{
    struct gpio_chip *chip;
    unsigned int offset;
    int ret;

    if (gpio >= GPIO_MAX_GPIOS)
        return -1;

    spinlock_acquire(&gpiolib_lock);

    if (gpio_allocated[gpio]) {
        spinlock_release(&gpiolib_lock);
        return -1;  /* already allocated */
    }

    if (gpio_to_chip_offset(gpio, &chip, &offset) < 0) {
        spinlock_release(&gpiolib_lock);
        return -1;
    }

    if (chip->ops && chip->ops->request)
        ret = chip->ops->request(chip, offset);
    else
        ret = 0;

    if (ret == 0) {
        gpio_allocated[gpio] = 1;
        if (label) {
            size_t len = strlen(label);
            if (len >= GPIO_NAME_MAX) len = GPIO_NAME_MAX - 1;
            for (size_t i = 0; i < len; i++)
                gpio_labels[gpio][i] = label[i];
            gpio_labels[gpio][len] = '\0';
        }
    }

    spinlock_release(&gpiolib_lock);
    return ret;
}

void gpio_free(unsigned int gpio)
{
    struct gpio_chip *chip;
    unsigned int offset;

    if (gpio >= GPIO_MAX_GPIOS)
        return;

    spinlock_acquire(&gpiolib_lock);

    if (!gpio_allocated[gpio]) {
        spinlock_release(&gpiolib_lock);
        return;
    }

    if (gpio_to_chip_offset(gpio, &chip, &offset) == 0) {
        if (chip->ops && chip->ops->free)
            chip->ops->free(chip, offset);
    }

    gpio_allocated[gpio] = 0;
    gpio_labels[gpio][0] = '\0';
    spinlock_release(&gpiolib_lock);
}

int gpio_direction_input(unsigned int gpio)
{
    struct gpio_chip *chip;
    unsigned int offset;

    if (gpio >= GPIO_MAX_GPIOS || !gpio_allocated[gpio])
        return -1;

    spinlock_acquire(&gpiolib_lock);
    int ret = -1;
    if (gpio_to_chip_offset(gpio, &chip, &offset) == 0) {
        if (chip->ops && chip->ops->direction_input)
            ret = chip->ops->direction_input(chip, offset);
    }
    spinlock_release(&gpiolib_lock);
    return ret;
}

int gpio_direction_output(unsigned int gpio, int value)
{
    struct gpio_chip *chip;
    unsigned int offset;

    if (gpio >= GPIO_MAX_GPIOS || !gpio_allocated[gpio])
        return -1;

    spinlock_acquire(&gpiolib_lock);
    int ret = -1;
    if (gpio_to_chip_offset(gpio, &chip, &offset) == 0) {
        if (chip->ops && chip->ops->direction_output)
            ret = chip->ops->direction_output(chip, offset, value);
    }
    spinlock_release(&gpiolib_lock);
    return ret;
}

int gpio_get_value(unsigned int gpio)
{
    struct gpio_chip *chip;
    unsigned int offset;

    if (gpio >= GPIO_MAX_GPIOS || !gpio_allocated[gpio])
        return -1;

    spinlock_acquire(&gpiolib_lock);
    int val = -1;
    if (gpio_to_chip_offset(gpio, &chip, &offset) == 0) {
        if (chip->ops && chip->ops->get)
            val = chip->ops->get(chip, offset);
    }
    spinlock_release(&gpiolib_lock);
    return val;
}

void gpio_set_value(unsigned int gpio, int value)
{
    struct gpio_chip *chip;
    unsigned int offset;

    if (gpio >= GPIO_MAX_GPIOS || !gpio_allocated[gpio])
        return;

    spinlock_acquire(&gpiolib_lock);
    if (gpio_to_chip_offset(gpio, &chip, &offset) == 0) {
        if (chip->ops && chip->ops->set)
            chip->ops->set(chip, offset, value);
    }
    spinlock_release(&gpiolib_lock);
}

int gpiochip_add(struct gpio_chip *chip)
{
    if (!chip || gpio_chip_count >= GPIO_MAX_CHIPS)
        return -1;

    spinlock_acquire(&gpiolib_lock);
    gpio_chips[gpio_chip_count++] = chip;
    spinlock_release(&gpiolib_lock);
    return 0;
}

void gpiolib_init(void)
{
    spinlock_init(&gpiolib_lock);

    for (int i = 0; i < GPIO_MAX_GPIOS; i++) {
        gpio_allocated[i] = 0;
        gpio_labels[i][0] = '\0';
    }

    kprintf("[OK] gpiolib: GPIO library initialised\n");
}

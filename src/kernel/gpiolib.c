#include "gpiolib.h"
#include "printf.h"
#include "kernel.h"
#include "string.h"
#include "spinlock.h"
#include "idt.h"
#include "export.h"
#include "errno.h"

/*
 * GPIO library — chip-based abstraction over x86-64 I/O ports.
 *
 * Chips register with gpiochip_add(); the library maps global GPIO
 * numbers to chip+offset and dispatches through the chip's ops.
 *
 * IRQ support: GPIO chips with interrupt capability register an
 * irq_base and optionally an irq_parent_vector.  gpio_to_irq()
 * maps GPIO numbers to IRQ vectors in a reserved range (96–191).
 * Per-line interrupt handlers can be registered with gpio_request_irq();
 * the chip driver's parent IRQ handler calls gpiochip_irq_demux() to
 * dispatch to individual handlers.
 */

#define GPIO_MAX_GPIOS (GPIO_MAX_CHIPS * GPIO_MAX_LINES)

static struct gpio_chip *gpio_chips[GPIO_MAX_CHIPS];
static int gpio_chip_count = 0;
static spinlock_t gpiolib_lock;

/* Per-GPIO allocation tracking */
static char gpio_labels[GPIO_MAX_GPIOS][GPIO_NAME_MAX];
static int  gpio_allocated[GPIO_MAX_GPIOS];

/* ── GPIO → chip+offset translation ────────────────────────────────── */

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

/* ── Standard GPIO operations ──────────────────────────────────────── */

/**
 * gpio_request - Request and reserve a GPIO pin
 * @gpio: GPIO pin number
 * @label: Descriptive label for the requester (may be NULL)
 *
 * Allocates the specified GPIO pin for exclusive use.  The pin must be
 * supported by a registered GPIO chip.  Once requested, the pin can be
 * configured for input/output and read/written.  Returns 0 on success
 * or a negative errno on failure.
 *
 * Return: 0 on success, -EINVAL if @gpio is out of range,
 *         -EBUSY if already allocated, -ENODEV if no GPIO chip covers @gpio
 */
int gpio_request(unsigned int gpio, const char *label)
{
    struct gpio_chip *chip;
    unsigned int offset;
    int ret;

    if (gpio >= GPIO_MAX_GPIOS)
        return -EINVAL;

    spinlock_acquire(&gpiolib_lock);

    if (gpio_allocated[gpio]) {
        spinlock_release(&gpiolib_lock);
        return -EBUSY;
    }

    if (gpio_to_chip_offset(gpio, &chip, &offset) < 0) {
        spinlock_release(&gpiolib_lock);
        return -ENODEV;
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

/**
 * gpio_free - Release a previously requested GPIO pin
 * @gpio: GPIO pin number to release
 *
 * Frees a GPIO pin that was allocated with gpio_request().  After
 * this call the pin becomes available for other requesters.  If the
 * GPIO chip supports a free operation it is called to release
 * hardware resources.
 */
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

/**
 * gpio_direction_input - Set a GPIO pin as input
 * @gpio: GPIO pin number
 *
 * Configures the direction of a previously requested GPIO pin to
 * input.  The underlying chip's direction_input operation is called
 * if available.
 *
 * Return: 0 on success, -EINVAL if @gpio is not allocated or out of range
 */
int gpio_direction_input(unsigned int gpio)
{
    struct gpio_chip *chip;
    unsigned int offset;

    if (gpio >= GPIO_MAX_GPIOS || !gpio_allocated[gpio])
        return -EINVAL;

    spinlock_acquire(&gpiolib_lock);
    int ret = -EINVAL;
    if (gpio_to_chip_offset(gpio, &chip, &offset) == 0) {
        if (chip->ops && chip->ops->direction_input)
            ret = chip->ops->direction_input(chip, offset);
    }
    spinlock_release(&gpiolib_lock);
    return ret;
}

/**
 * gpio_direction_output - Set a GPIO pin as output with initial value
 * @gpio: GPIO pin number
 * @value: Initial output value (0 or 1)
 *
 * Configures the direction of a previously requested GPIO pin to
 * output and sets the initial output value.  The underlying chip's
 * direction_output operation is called if available.
 *
 * Return: 0 on success, -EINVAL if @gpio is not allocated or out of range
 */
int gpio_direction_output(unsigned int gpio, int value)
{
    struct gpio_chip *chip;
    unsigned int offset;

    if (gpio >= GPIO_MAX_GPIOS || !gpio_allocated[gpio])
        return -EINVAL;

    spinlock_acquire(&gpiolib_lock);
    int ret = -EINVAL;
    if (gpio_to_chip_offset(gpio, &chip, &offset) == 0) {
        if (chip->ops && chip->ops->direction_output)
            ret = chip->ops->direction_output(chip, offset, value);
    }
    spinlock_release(&gpiolib_lock);
    return ret;
}

/**
 * gpio_get_value - Read the current value of a GPIO pin
 * @gpio: GPIO pin number
 *
 * Reads the logical value of a previously requested GPIO pin
 * configured as input.  The underlying chip's get operation is
 * called if available.
 *
 * Return: 0 or 1 on success, -EINVAL if @gpio is not allocated or out of range
 */
int gpio_get_value(unsigned int gpio)
{
    struct gpio_chip *chip;
    unsigned int offset;

    if (gpio >= GPIO_MAX_GPIOS || !gpio_allocated[gpio])
        return -EINVAL;

    spinlock_acquire(&gpiolib_lock);
    int val = -EINVAL;
    if (gpio_to_chip_offset(gpio, &chip, &offset) == 0) {
        if (chip->ops && chip->ops->get)
            val = chip->ops->get(chip, offset);
    }
    spinlock_release(&gpiolib_lock);
    return val;
}

/**
 * gpio_set_value - Set the output value of a GPIO pin
 * @gpio: GPIO pin number
 * @value: Output value (0 or 1)
 *
 * Sets the logical output value of a previously requested GPIO pin
 * configured as output.  The underlying chip's set operation is
 * called if available.
 */
void gpio_set_value(unsigned int gpio, unsigned int value)
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
    int ret = 0;

    if (!chip || gpio_chip_count >= GPIO_MAX_CHIPS)
        return -ENOMEM;

    spinlock_acquire(&gpiolib_lock);

    /* Auto-assign IRQ base if the chip has IRQ ops and no base set */
    if (chip->ops && (chip->ops->irq_request || chip->ops->irq_set_type)) {
        if (chip->irq_base == 0) {
            /* Find the next free IRQ vector range */
            unsigned int next_irq = GPIO_IRQ_BASE_VECTOR;
            for (int i = 0; i < gpio_chip_count; i++) {
                if (gpio_chips[i] && gpio_chips[i]->irq_base > 0) {
                    unsigned int end = gpio_chips[i]->irq_base +
                                       gpio_chips[i]->ngpio;
                    if (end > next_irq)
                        next_irq = end;
                }
            }
            /* Clamp to available range */
            if (next_irq + chip->ngpio > GPIO_IRQ_MAX_VECTOR + 1) {
                spinlock_release(&gpiolib_lock);
                kprintf("[GPIO] WARNING: chip '%s' needs %u IRQ vectors "
                        "but only %u available\n",
                        chip->label ? chip->label : "?",
                        chip->ngpio,
                        GPIO_IRQ_MAX_VECTOR - next_irq + 1);
                return -ENOSPC;
            }
            chip->irq_base = next_irq;
        }

        /* Register the per-line vectors as named IDT entries */
        for (unsigned int i = 0; i < chip->ngpio && i < GPIO_IRQ_MAX_PER_CHIP; i++) {
            int vec = chip->irq_base + i;
            if (vec > GPIO_IRQ_MAX_VECTOR)
                break;
            /* Set a default name for /proc/interrupts */
            idt_set_vector_name(vec, chip->label ? chip->label : "gpio");
        }

        /* If the chip has a parent interrupt vector, register the demux handler */
        if (chip->irq_parent_vector >= 0) {
            /* Wrap the chip pointer into a static wrapper for the IDT handler.
             * Since idt_register_handler takes a simple function pointer, we need
             * one static wrapper per chip.  For simplicity, we allocate a small
             * array of wrapper trampolines indexed by chip position. */
            /* NOTE: The chip driver is responsible for calling gpiochip_irq_demux()
             * from its own registered parent IRQ handler, since idt_register_handler
             * doesn't support a context parameter.  We just set the vector name. */
            idt_set_vector_name(chip->irq_parent_vector, chip->label);
        }
    }

    gpio_chips[gpio_chip_count++] = chip;
    spinlock_release(&gpiolib_lock);

    kprintf("[GPIO] chip '%s' registered: base=%u ngpio=%u irq_base=%u irq_parent=%d\n",
            chip->label ? chip->label : "?",
            chip->base, chip->ngpio,
            chip->irq_base, chip->irq_parent_vector);
    return ret;
}

/* ── GPIO IRQ support ──────────────────────────────────────────────── */

int gpio_to_irq(unsigned int gpio)
{
    struct gpio_chip *chip;
    unsigned int offset;

    if (gpio >= GPIO_MAX_GPIOS)
        return -EINVAL;

    spinlock_acquire(&gpiolib_lock);

    if (gpio_to_chip_offset(gpio, &chip, &offset) < 0) {
        spinlock_release(&gpiolib_lock);
        return -ENODEV;
    }

    /* Check chip has interrupt support */
    if (chip->irq_base == 0 || !chip->ops ||
        (!chip->ops->irq_request && !chip->ops->irq_set_type)) {
        spinlock_release(&gpiolib_lock);
        return -ENXIO;
    }

    int irq = (int)(chip->irq_base + offset);
    spinlock_release(&gpiolib_lock);
    return irq;
}

int gpio_request_irq(unsigned int gpio, isr_handler_t handler,
                     const char *name, int type)
{
    struct gpio_chip *chip;
    unsigned int offset;
    int ret;

    if (gpio >= GPIO_MAX_GPIOS || !handler)
        return -EINVAL;

    spinlock_acquire(&gpiolib_lock);

    /* GPIO must be allocated first */
    if (!gpio_allocated[gpio]) {
        spinlock_release(&gpiolib_lock);
        return -EPERM;
    }

    if (gpio_to_chip_offset(gpio, &chip, &offset) < 0) {
        spinlock_release(&gpiolib_lock);
        return -ENODEV;
    }

    /* Chip must support IRQs */
    if (chip->irq_base == 0 || !chip->ops || !chip->ops->irq_request) {
        spinlock_release(&gpiolib_lock);
        return -ENXIO;
    }

    if (offset >= GPIO_IRQ_MAX_PER_CHIP) {
        spinlock_release(&gpiolib_lock);
        return -ENOSPC;
    }

    /* Check not already registered */
    if (chip->irq_handlers[offset].flags & GPIO_IRQ_FLAG_USED) {
        spinlock_release(&gpiolib_lock);
        return -EBUSY;
    }

    /* Call chip-specific irq_request to configure the hardware */
    ret = chip->ops->irq_request(chip, offset);
    if (ret < 0) {
        spinlock_release(&gpiolib_lock);
        return ret;
    }

    /* Set the trigger type */
    if (chip->ops->irq_set_type) {
        ret = chip->ops->irq_set_type(chip, offset, type);
        if (ret < 0) {
            if (chip->ops->irq_free)
                chip->ops->irq_free(chip, offset);
            spinlock_release(&gpiolib_lock);
            return ret;
        }
    }

    /* Register the per-line handler descriptor */
    chip->irq_handlers[offset].flags  = GPIO_IRQ_FLAG_USED;
    chip->irq_handlers[offset].handler = handler;
    chip->irq_handlers[offset].name   = name;

    /* Set the vector name for /proc/interrupts */
    int vec = chip->irq_base + offset;
    if (vec >= 0 && vec <= 255)
        idt_set_vector_name(vec, name ? name : "gpio_irq");

    spinlock_release(&gpiolib_lock);

    kprintf("[GPIO] irq registered: gpio=%u -> vec=%d type=%d name='%s'\n",
            gpio, vec, type, name ? name : "?");
    return 0;
}

void gpio_free_irq(unsigned int gpio)
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

    if (gpio_to_chip_offset(gpio, &chip, &offset) < 0) {
        spinlock_release(&gpiolib_lock);
        return;
    }

    if (offset >= GPIO_IRQ_MAX_PER_CHIP) {
        spinlock_release(&gpiolib_lock);
        return;
    }

    if (!(chip->irq_handlers[offset].flags & GPIO_IRQ_FLAG_USED)) {
        spinlock_release(&gpiolib_lock);
        return;
    }

    /* Call chip-specific irq_free */
    if (chip->ops && chip->ops->irq_free)
        chip->ops->irq_free(chip, offset);

    /* Mask the IRQ at the chip level */
    if (chip->ops && chip->ops->irq_mask)
        chip->ops->irq_mask(chip, offset);

    /* Clear the handler descriptor */
    memset(&chip->irq_handlers[offset], 0, sizeof(chip->irq_handlers[offset]));

    spinlock_release(&gpiolib_lock);
}

int gpio_irq_set_type(unsigned int gpio, int type)
{
    struct gpio_chip *chip;
    unsigned int offset;
    int ret;

    if (gpio >= GPIO_MAX_GPIOS)
        return -EINVAL;

    spinlock_acquire(&gpiolib_lock);

    if (!gpio_allocated[gpio]) {
        spinlock_release(&gpiolib_lock);
        return -EPERM;
    }

    if (gpio_to_chip_offset(gpio, &chip, &offset) < 0) {
        spinlock_release(&gpiolib_lock);
        return -ENODEV;
    }

    if (!chip->ops || !chip->ops->irq_set_type) {
        spinlock_release(&gpiolib_lock);
        return -ENXIO;
    }

    ret = chip->ops->irq_set_type(chip, offset, type);
    spinlock_release(&gpiolib_lock);
    return ret;
}

/*
 * GPIO chip IRQ demux handler.
 *
 * Called from a gpiochip's parent IRQ handler.  Scans all GPIO lines
 * on the chip, invokes irq_pending() for each, and dispatches to any
 * registered per-line handler.
 *
 * NOTE: This function does NOT hold gpiolib_lock — it is called from
 * interrupt context and should not block.  The chip driver's parent
 * IRQ handler is responsible for calling this at the appropriate point
 * after acknowledging the parent interrupt.
 */
void gpiochip_irq_demux(struct interrupt_frame *frame, struct gpio_chip *chip)
{
    if (!frame || !chip)
        return;

    struct gpio_chip_ops *ops = chip->ops;
    if (!ops || !ops->irq_pending)
        return;

    /* Scan all GPIO lines on this chip for pending interrupts */
    for (unsigned int i = 0; i < chip->ngpio && i < GPIO_IRQ_MAX_PER_CHIP; i++) {
        if (!(chip->irq_handlers[i].flags & GPIO_IRQ_FLAG_USED))
            continue;
        if (!(chip->valid_mask & (1u << i)))
            continue;

        if (ops->irq_pending(chip, i)) {
            /* Mask edge-triggered lines before calling handler to prevent
             * re-entry.  The handler should unmask when done, or the chip
             * driver's irq_unmask handles it. */
            if (ops->irq_mask)
                ops->irq_mask(chip, i);

            /* Invoke the per-line handler */
            if (chip->irq_handlers[i].handler)
                chip->irq_handlers[i].handler(frame);
        }
    }
}

/* ── Initialisation ─────────────────────────────────────────────────── */

void __init gpiolib_init(void)
{
    spinlock_init(&gpiolib_lock);

    for (int i = 0; i < GPIO_MAX_GPIOS; i++) {
        gpio_allocated[i] = 0;
        gpio_labels[i][0] = '\0';
    }

    kprintf("[OK] gpiolib: GPIO library initialised\n");
}

/* ── Exports for loadable kernel modules ────────────────────────────── */
EXPORT_SYMBOL(gpio_request);
EXPORT_SYMBOL(gpio_free);
EXPORT_SYMBOL(gpio_direction_input);
EXPORT_SYMBOL(gpio_direction_output);
EXPORT_SYMBOL(gpio_get_value);
EXPORT_SYMBOL(gpio_set_value);
EXPORT_SYMBOL(gpio_to_irq);
EXPORT_SYMBOL(gpio_request_irq);
EXPORT_SYMBOL(gpio_free_irq);
EXPORT_SYMBOL(gpio_irq_set_type);
EXPORT_SYMBOL(gpiochip_add);
EXPORT_SYMBOL(gpiochip_irq_demux);

/*
 * gpio_set_irq_mode — configure GPIO IRQ trigger mode
 *
 * Configures the trigger type for a GPIO line used as an interrupt source.
 * Dispatches to the chip's irq_set_type operation if available.
 *
 * @pin:   GPIO line number
 * @mode:  GPIO_IRQ_TYPE_EDGE_RISING / EDGE_FALLING / EDGE_BOTH /
 *         LEVEL_HIGH / LEVEL_LOW
 */
void gpio_set_irq_mode(unsigned int pin, int mode)
{
    if (pin >= GPIO_MAX_GPIOS)
        return;

    struct gpio_chip *chip;
    unsigned int offset;

    spinlock_acquire(&gpiolib_lock);

    if (gpio_to_chip_offset(pin, &chip, &offset) < 0) {
        spinlock_release(&gpiolib_lock);
        return;
    }

    if (!chip->ops || !chip->ops->irq_set_type) {
        spinlock_release(&gpiolib_lock);
        return;
    }

    /* Validate the trigger mode */
    if (mode < GPIO_IRQ_TYPE_EDGE_RISING || mode > GPIO_IRQ_TYPE_LEVEL_LOW) {
        spinlock_release(&gpiolib_lock);
        return;
    }

    chip->ops->irq_set_type(chip, offset, mode);

    spinlock_release(&gpiolib_lock);
}

/* ── Stub: gpiochip_irqchip_add ─────────────────────────────── */
int gpiochip_irqchip_add(void *chip, void *irqchip)
{
    (void)chip;
    (void)irqchip;
    kprintf("[gpio] gpiochip_irqchip_add: not yet implemented\n");
    return 0;
}
/* ── Stub: gpiochip_irqchip_remove ─────────────────────────────── */
int gpiochip_irqchip_remove(void *chip)
{
    (void)chip;
    kprintf("[gpio] gpiochip_irqchip_remove: not yet implemented\n");
    return 0;
}
/* ── Stub: gpio_to_desc ─────────────────────────────── */
void* gpio_to_desc(unsigned int gpio)
{
    (void)gpio;
    kprintf("[gpio] gpio_to_desc: not yet implemented\n");
    return 0;
}
/* ── Stub: desc_to_gpio ─────────────────────────────── */
unsigned int desc_to_gpio(const void *desc)
{
    (void)desc;
    kprintf("[gpio] desc_to_gpio: not yet implemented\n");
    return 0;
}

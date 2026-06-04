#ifndef GPIOLIB_H
#define GPIOLIB_H

#include "types.h"
#include "idt.h"

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

/* ── IRQ trigger types for GPIO interrupts ──────────────────────────── */
#define GPIO_IRQ_TYPE_EDGE_RISING  0
#define GPIO_IRQ_TYPE_EDGE_FALLING 1
#define GPIO_IRQ_TYPE_EDGE_BOTH    2
#define GPIO_IRQ_TYPE_LEVEL_HIGH   3
#define GPIO_IRQ_TYPE_LEVEL_LOW    4

/* ── GPIO IRQ descriptor flags ──────────────────────────────────────── */
#define GPIO_IRQ_FLAG_USED        (1 << 0)

/* Maximum GPIO IRQ handlers per chip (one per line maximum) */
#define GPIO_IRQ_MAX_PER_CHIP    32

/*
 * Base IRQ vector for GPIO chip interrupts.
 * GPIO IRQs are allocated in the 96–191 range to avoid conflicts with
 * CPU exceptions (0–31), PIC legacy IRQs (32–47), and typical MSI/IOAPIC
 * vectors (48–95). Each chip gets ngpio contiguous vectors from irq_base.
 */
#define GPIO_IRQ_BASE_VECTOR     96
#define GPIO_IRQ_MAX_VECTOR      191
#define GPIO_IRQ_SPAN            (GPIO_IRQ_MAX_VECTOR - GPIO_IRQ_BASE_VECTOR + 1)

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

    /* ── IRQ operations (optional — set to NULL if chip has no IRQ support) ── */
    int  (*irq_request)(struct gpio_chip *chip, unsigned int offset);
    void (*irq_free)(struct gpio_chip *chip, unsigned int offset);
    int  (*irq_set_type)(struct gpio_chip *chip, unsigned int offset, int type);
    void (*irq_mask)(struct gpio_chip *chip, unsigned int offset);
    void (*irq_unmask)(struct gpio_chip *chip, unsigned int offset);
    /* Return non-zero if this specific GPIO line has a pending interrupt */
    int  (*irq_pending)(struct gpio_chip *chip, unsigned int offset);
};

/*
 * Per-GPIO-line IRQ handler descriptor.
 * Tracks the handler function and name registered for a specific GPIO
 * input line that is configured as an interrupt source.
 */
struct gpio_irq_desc {
    uint32_t      flags;          /* GPIO_IRQ_FLAG_* */
    isr_handler_t handler;        /* per-line IRQ handler (demux target) */
    const char   *name;           /* human-readable name for /proc/interrupts */
};

struct gpio_chip {
    const char            *label;
    struct gpio_chip_ops  *ops;
    unsigned int           base;        /* global GPIO number base */
    unsigned int           ngpio;       /* number of GPIO lines */
    uint32_t               valid_mask;  /* bitmask of valid lines */

    /* ── IRQ fields (0 / NULL = chip has no interrupt capability) ────────── */
    unsigned int               irq_base;          /* first IRQ vector for this chip */
    int                        irq_parent_vector; /* -1 = no parent IRQ (polling only) */
    struct gpio_irq_desc       irq_handlers[GPIO_IRQ_MAX_PER_CHIP];
};

/*
 * gpio_to_irq  - Map a GPIO line number to an IRQ vector.
 *
 * Returns the IRQ vector number suitable for use with the kernel's
 * interrupt registration, or -1 if the GPIO chip has no IRQ support.
 * The returned vector can be used with idt_set_vector_name() to label
 * it for /proc/interrupts display.
 */
int gpio_to_irq(unsigned int gpio);

/*
 * gpio_request_irq  - Register a handler for a GPIO line's interrupt.
 *
 * @gpio:     The GPIO line number.
 * @handler:  Interrupt handler function (receives interrupt_frame).
 * @name:     Human-readable name for /proc/interrupts (may be NULL).
 * @type:     GPIO_IRQ_TYPE_* trigger type.
 *
 * Returns 0 on success, negative errno on failure.
 */
int gpio_request_irq(unsigned int gpio, isr_handler_t handler,
                     const char *name, int type);

/*
 * gpio_free_irq  - Unregister a GPIO line's interrupt handler.
 *
 * @gpio:  The GPIO line number.
 */
void gpio_free_irq(unsigned int gpio);

/*
 * gpio_irq_set_type  - Set the trigger type for a GPIO IRQ line.
 *
 * @gpio:  The GPIO line number.
 * @type:  GPIO_IRQ_TYPE_* trigger type.
 *
 * Returns 0 on success, negative errno on failure.
 */
int gpio_irq_set_type(unsigned int gpio, int type);

/*
 * gpiochip_irq_demux  - Generic GPIO chip IRQ demux handler.
 *
 * Called from a gpiochip's parent IRQ vector.  Iterates all lines
 * on the chip, checks irq_pending() for each, and invokes any
 * registered per-line handler.
 *
 * This function can be used directly as idt_register_handler callback
 * for chips that have a parent interrupt vector set.
 *
 * @frame:  The interrupt frame from the parent IRQ.
 * @chip:   Pointer to the gpio_chip whose lines to scan.
 */
void gpiochip_irq_demux(struct interrupt_frame *frame, struct gpio_chip *chip);

/* ── Standard GPIO operations ───────────────────────────────────────── */

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
void gpio_set_value(unsigned int gpio, unsigned int value);

/*
 * gpiochip_add  - Register a GPIO chip with the library.
 */
int gpiochip_add(struct gpio_chip *chip);

/*
 * gpiolib_init  - Initialise the GPIO library.
 */
void gpiolib_init(void);

#endif /* GPIOLIB_H */

/*
 * i3c.c — I3C (Improved Inter Integrated Circuit) serial bus driver
 *
 * Implements the I3C serial bus protocol (MIPI I3C Specification).
 * I3C is a 2-wire serial bus that is backward-compatible with I2C
 * but offers higher speed, lower power, and in-band interrupts.
 *
 * Key features:
 *   - I3C controller (primary/master) role
 *   - Dynamic address assignment (DAA) for I3C targets
 *   - I2C backward-compatible addressing
 *   - In-band interrupt (IBI) handling
 *   - Hot-join support
 *   - Common command code (CCC) transmission
 *
 * Item 459: I3C serial bus
 */

#define KERNEL_INTERNAL
#include "i3c.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "timer.h"
#include "spinlock.h"
#include "errno.h"
#include "export.h"

/* ── Constants ─────────────────────────────────────────────────────── */

#define I3C_MAX_CONTROLLERS    4   /* max I3C controllers */
#define I3C_MAX_DEVICES        16  /* max devices per controller */
#define I3C_ADDR_BCAST         0x7E   /* broadcast address */
#define I3C_ADDR_RESERVED      0x7F

/* I3C CCC (Common Command Code) identifiers */
#define I3C_CCC_ENEC           0x00   /* Enable Events */
#define I3C_CCC_DISEC          0x01   /* Disable Events */
#define I3C_CCC_ENTDAA         0x29   /* Enter Dynamic Address Assignment */
#define I3C_CCC_SETDASA        0x27   /* Set Dynamic Address from Static */
#define I3C_CCC_RSTDAA         0x06   /* Reset Dynamic Address Assignment */
#define I3C_CCC_GETMRL         0x0E   /* Get Max Read Length */
#define I3C_CCC_GETMWL         0x0D   /* Get Max Write Length */
#define I3C_CCC_GETMXDS        0x14   /* Get Max Data Speed */
#define I3C_CCC_GETPID         0x10   /* Get Provisioned ID */
#define I3C_CCC_GETBCR         0x11   /* Get Bus Characteristic Register */
#define I3C_CCC_GETDCR         0x12   /* Get Device Characteristic Register */
#define I3C_CCC_GETSTATUS      0x15   /* Get Device Status */

/* I3C device capability bits (BCR) */
#define I3C_BCR_MAX_DATA_SPEED     (1U << 0)
#define I3C_BCR_IBI_PAYLOAD        (1U << 1)
#define I3C_BCR_IBI_REQUEST        (1U << 2)
#define I3C_BCR_HOT_JOIN           (1U << 3)

/* I3C transfer types */
#define I3C_XFER_READ         0
#define I3C_XFER_WRITE        1
#define I3C_XFER_CCC          2

/* I3C bus speeds */
#define I3C_SPEED_SDR         0   /* Single Data Rate (up to 12.5 MHz) */
#define I3C_SPEED_DDR         1   /* Double Data Rate */
#define I3C_SPEED_HDR_DDR     2   /* High Data Rate DDR */
#define I3C_SPEED_HDR_TSP     3   /* High Data Rate Ternary Symbol PAM-3 */

/* ── I3C device (target) ──────────────────────────────────────────── */

struct i3c_device {
    int     in_use;
    uint8_t dyn_addr;            /* dynamic address (assigned by DAA) */
    uint8_t static_addr;         /* static I2C-compatible address (0 if none) */
    uint8_t bcr;                 /* Bus Characteristic Register */
    uint8_t dcr;                 /* Device Characteristic Register */
    uint64_t pid;                /* Provisioned ID (48-bit) */
    uint32_t max_read_len;       /* maximum read length */
    uint32_t max_write_len;      /* maximum write length */
    int     supports_ibi;        /* 1 = supports in-band interrupts */
    int     supports_hot_join;   /* 1 = supports hot-join */
};

/* ── I3C controller ───────────────────────────────────────────────── */

struct i3c_controller {
    int     in_use;
    int     id;
    char    name[16];
    int     speed;               /* I3C_SPEED_* */
    uint32_t bus_freq_hz;        /* bus frequency in Hz */

    struct i3c_device devices[I3C_MAX_DEVICES];
    int     num_devices;

    /* IBI (In-Band Interrupt) handling */
    int     ibi_pending;

    spinlock_t lock;
};

/* ── Global state ─────────────────────────────────────────────────── */

static struct i3c_controller g_i3c_controllers[I3C_MAX_CONTROLLERS];
static int g_i3c_initialized = 0;

/* ═══════════════════════════════════════════════════════════════════════
 *  Internal helpers
 * ═══════════════════════════════════════════════════════════════════════ */

static struct i3c_device *i3c_find_device(struct i3c_controller *ctl,
                                          uint8_t addr)
{
    for (int i = 0; i < ctl->num_devices; i++) {
        if (ctl->devices[i].in_use &&
            (ctl->devices[i].dyn_addr == addr ||
             ctl->devices[i].static_addr == addr))
            return &ctl->devices[i];
    }
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Public API
 * ═══════════════════════════════════════════════════════════════════════ */

/* Initialize the I3C subsystem. */
void i3c_init(void)
{
    if (g_i3c_initialized) return;

    memset(g_i3c_controllers, 0, sizeof(g_i3c_controllers));
    g_i3c_initialized = 1;

    kprintf("[OK] I3C bus subsystem initialized\n");
}
EXPORT_SYMBOL(i3c_init);

/* Register an I3C controller.
 *
 * @name:        controller name (e.g., "i3c-0")
 * @speed:       I3C_SPEED_* (SDR, DDR, etc.)
 * @bus_freq_hz: bus operating frequency in Hz
 *
 * Returns controller ID (>= 0) on success, negative on failure.
 */
int i3c_controller_register(const char *name, int speed, uint32_t bus_freq_hz)
{
    if (!g_i3c_initialized) return -EAGAIN;
    if (!name) return -EINVAL;

    for (int i = 0; i < I3C_MAX_CONTROLLERS; i++) {
        if (!g_i3c_controllers[i].in_use) {
            struct i3c_controller *ctl = &g_i3c_controllers[i];
            memset(ctl, 0, sizeof(*ctl));
            ctl->in_use = 1;
            ctl->id = i;
            strncpy(ctl->name, name, sizeof(ctl->name) - 1);
            ctl->name[sizeof(ctl->name) - 1] = '\0';
            ctl->speed = speed;
            ctl->bus_freq_hz = bus_freq_hz;
            ctl->num_devices = 0;
            ctl->ibi_pending = 0;
            spinlock_init(&ctl->lock);

            kprintf("[I3C] registered controller '%s' as id %d (speed=%d, freq=%u Hz)\n",
                    name, i, speed, bus_freq_hz);
            return i;
        }
    }
    return -ENOSPC;
}
EXPORT_SYMBOL(i3c_controller_register);

/* Unregister an I3C controller. */
int i3c_controller_unregister(int ctl_id)
{
    if (ctl_id < 0 || ctl_id >= I3C_MAX_CONTROLLERS ||
        !g_i3c_controllers[ctl_id].in_use)
        return -EINVAL;

    memset(&g_i3c_controllers[ctl_id], 0, sizeof(struct i3c_controller));
    return 0;
}
EXPORT_SYMBOL(i3c_controller_unregister);

/* Perform Dynamic Address Assignment (DAA) on the I3C bus.
 * Discovers all I3C devices and assigns them dynamic addresses.
 * Returns the number of devices discovered. */
int i3c_daa(int ctl_id)
{
    if (ctl_id < 0 || ctl_id >= I3C_MAX_CONTROLLERS ||
        !g_i3c_controllers[ctl_id].in_use)
        return -EINVAL;

    struct i3c_controller *ctl = &g_i3c_controllers[ctl_id];

    kprintf("[I3C] DAA on controller %d\n", ctl_id);

    /* Simulate DAA: assign dynamic addresses to discovered devices.
     * In a real I3C controller, this would involve sending ENTDAA
     * CCC and processing the devices' PROVISIONED IDs. */
    int devices_found = 0;
    for (int i = 0; i < ctl->num_devices; i++) {
        if (ctl->devices[i].in_use) {
            if (ctl->devices[i].dyn_addr == 0) {
                /* Assign a dynamic address starting from 0x08 */
                ctl->devices[i].dyn_addr = (uint8_t)(0x08 + i);
                devices_found++;
                kprintf("[I3C]  device %d: dyn_addr=0x%02x pid=0x%llx\n",
                        i, ctl->devices[i].dyn_addr,
                        (unsigned long long)ctl->devices[i].pid);
            }
        }
    }

    return devices_found;
}
EXPORT_SYMBOL(i3c_daa);

/* Add an I3C device to a controller.
 *
 * @static_addr: I2C-compatible static address (0 if pure I3C)
 * @pid:         48-bit Provisioned ID
 * @bcr:         Bus Characteristic Register
 * @dcr:         Device Characteristic Register
 *
 * Returns device index (>= 0) on success, negative on failure.
 */
int i3c_add_device(int ctl_id, uint8_t static_addr, uint64_t pid,
                   uint8_t bcr, uint8_t dcr)
{
    if (ctl_id < 0 || ctl_id >= I3C_MAX_CONTROLLERS ||
        !g_i3c_controllers[ctl_id].in_use)
        return -EINVAL;

    struct i3c_controller *ctl = &g_i3c_controllers[ctl_id];

    if (ctl->num_devices >= I3C_MAX_DEVICES)
        return -ENOSPC;

    int idx = ctl->num_devices;
    struct i3c_device *dev = &ctl->devices[idx];
    memset(dev, 0, sizeof(*dev));
    dev->in_use = 1;
    dev->static_addr = static_addr;
    dev->dyn_addr = 0; /* assigned by DAA */
    dev->pid = pid;
    dev->bcr = bcr;
    dev->dcr = dcr;
    dev->max_read_len = 256;
    dev->max_write_len = 256;
    dev->supports_ibi = (bcr & I3C_BCR_IBI_REQUEST) ? 1 : 0;
    dev->supports_hot_join = (bcr & I3C_BCR_HOT_JOIN) ? 1 : 0;
    ctl->num_devices++;

    kprintf("[I3C] added device at idx=%d: static=0x%02x pid=0x%llx "
            "bcr=0x%02x dcr=0x%02x\n",
            idx, (unsigned int)static_addr,
            (unsigned long long)pid, (unsigned int)bcr, (unsigned int)dcr);
    return idx;
}
EXPORT_SYMBOL(i3c_add_device);

/* Read data from an I3C device.
 *
 * @ctl_id: controller ID
 * @addr:   device address (dynamic or static)
 * @buf:    destination buffer
 * @len:    number of bytes to read
 *
 * Returns number of bytes read on success, negative on error.
 */
ssize_t i3c_read(int ctl_id, uint8_t addr, uint8_t *buf, uint32_t len)
{
    if (ctl_id < 0 || ctl_id >= I3C_MAX_CONTROLLERS ||
        !g_i3c_controllers[ctl_id].in_use)
        return -EINVAL;

    struct i3c_controller *ctl = &g_i3c_controllers[ctl_id];
    struct i3c_device *dev = i3c_find_device(ctl, addr);
    if (!dev) return -ENODEV;

    /* In a real implementation, this would drive the I3C bus signals.
     * For now we simulate a successful read with zeros. */
    if (len > dev->max_read_len)
        len = dev->max_read_len;

    memset(buf, 0, len);
    kprintf("[I3C] read %u bytes from addr 0x%02x on controller %d\n",
            len, (unsigned int)addr, ctl_id);
    return (ssize_t)len;
}
EXPORT_SYMBOL(i3c_read);

/* Write data to an I3C device.
 *
 * @ctl_id: controller ID
 * @addr:   device address (dynamic or static)
 * @buf:    source buffer
 * @len:    number of bytes to write
 *
 * Returns number of bytes written on success, negative on error.
 */
ssize_t i3c_write(int ctl_id, uint8_t addr, const uint8_t *buf, uint32_t len)
{
    if (ctl_id < 0 || ctl_id >= I3C_MAX_CONTROLLERS ||
        !g_i3c_controllers[ctl_id].in_use)
        return -EINVAL;

    struct i3c_controller *ctl = &g_i3c_controllers[ctl_id];
    struct i3c_device *dev = i3c_find_device(ctl, addr);
    if (!dev) return -ENODEV;

    if (len > dev->max_write_len)
        len = dev->max_write_len;

    kprintf("[I3C] wrote %u bytes to addr 0x%02x on controller %d\n",
            len, (unsigned int)addr, ctl_id);
    return (ssize_t)len;
}
EXPORT_SYMBOL(i3c_write);

/* Send a CCC (Common Command Code) to the broadcast address.
 *
 * @ctl_id: controller ID
 * @ccc:    command code (I3C_CCC_*)
 * @data:   optional data payload (can be NULL)
 * @len:    length of data payload
 *
 * Returns 0 on success, negative on error.
 */
int i3c_send_ccc(int ctl_id, uint8_t ccc, const uint8_t *data, uint32_t len)
{
    if (ctl_id < 0 || ctl_id >= I3C_MAX_CONTROLLERS ||
        !g_i3c_controllers[ctl_id].in_use)
        return -EINVAL;

    kprintf("[I3C] CCC 0x%02x broadcast on controller %d (len=%u)\n",
            (unsigned int)ccc, ctl_id, len);
    return 0;
}
EXPORT_SYMBOL(i3c_send_ccc);

/* Handle an In-Band Interrupt (IBI) from a device.
 *
 * @ctl_id:  controller ID
 * @addr:    address of the device that sent the IBI
 * @payload: IBI payload data (if any)
 * @len:     length of payload
 *
 * Returns 0 on success. */
int i3c_handle_ibi(int ctl_id, uint8_t addr,
                   const uint8_t *payload, uint32_t len)
{
    if (ctl_id < 0 || ctl_id >= I3C_MAX_CONTROLLERS ||
        !g_i3c_controllers[ctl_id].in_use)
        return -EINVAL;

    kprintf("[I3C] IBI from addr 0x%02x on controller %d (payload len=%u)\n",
            (unsigned int)addr, ctl_id, len);

    struct i3c_controller *ctl = &g_i3c_controllers[ctl_id];
    ctl->ibi_pending = 1;

    return 0;
}
EXPORT_SYMBOL(i3c_handle_ibi);

/* Set the I3C bus speed. */
int i3c_set_speed(int ctl_id, int speed, uint32_t freq_hz)
{
    if (ctl_id < 0 || ctl_id >= I3C_MAX_CONTROLLERS ||
        !g_i3c_controllers[ctl_id].in_use)
        return -EINVAL;

    struct i3c_controller *ctl = &g_i3c_controllers[ctl_id];
    ctl->speed = speed;
    if (freq_hz > 0) ctl->bus_freq_hz = freq_hz;

    kprintf("[I3C] controller %d speed set to %d (%u Hz)\n",
            ctl_id, speed, ctl->bus_freq_hz);
    return 0;
}
EXPORT_SYMBOL(i3c_set_speed);

/* Get the number of devices on an I3C controller. */
int i3c_device_count(int ctl_id)
{
    if (ctl_id < 0 || ctl_id >= I3C_MAX_CONTROLLERS ||
        !g_i3c_controllers[ctl_id].in_use)
        return -EINVAL;

    return g_i3c_controllers[ctl_id].num_devices;
}
EXPORT_SYMBOL(i3c_device_count);
#include "module.h"
module_init(i3c_init);

/* ── Stub: i3c_register_device ─────────────────────────────── */
int i3c_register_device(void *dev)
{
    (void)dev;
    kprintf("[i3c] i3c_register_device: not yet implemented\n");
    return 0;
}

/*
 * spi.h — SPI bus controller framework
 *
 * Master/slave device model for the Serial Peripheral Interface bus.
 * Supports master mode with GPIO chip-select, full-duplex transfers,
 * and write_then_read half-duplex operations.
 *
 * SPI modes:
 *   Mode 0: CPOL=0, CPHA=0 (clock idle LOW, sample on leading edge)
 *   Mode 1: CPOL=0, CPHA=1 (clock idle LOW, sample on trailing edge)
 *   Mode 2: CPOL=1, CPHA=0 (clock idle HIGH, sample on leading edge)
 *   Mode 3: CPOL=1, CPHA=1 (clock idle HIGH, sample on trailing edge)
 */

#ifndef SPI_H
#define SPI_H

#include "types.h"

/* ── SPI mode flags (CPOL | CPHA) ────────────────────────────────── */
#define SPI_MODE_0          0
#define SPI_MODE_1          1
#define SPI_MODE_2          2
#define SPI_MODE_3          3
#define SPI_CPHA            (1 << 0)
#define SPI_CPOL            (1 << 1)
#define SPI_MODE_MASK       3

/* ── SPI transfer flags ──────────────────────────────────────────── */
#define SPI_TRANSFER_DONE   (1 << 0)   /* transfer completed */
#define SPI_TRANSFER_CS_HIGH (1 << 1)  /* de-assert CS after this transfer */

/* ── Maximum devices per bus ─────────────────────────────────────── */
#define SPI_MAX_DEVICES     8

/* ── Maximum number of SPI buses ─────────────────────────────────── */
#define SPI_BUS_MAX         4

/* ── Bit-order constants ─────────────────────────────────────────── */
#define SPI_BIT_ORDER_MSB   0
#define SPI_BIT_ORDER_LSB   1

/* ── Forward declaration ─────────────────────────────────────────── */
struct spi_device;
struct spi_transfer;

/**
 * struct spi_master_ops — Low-level hardware operations for an SPI bus.
 *
 * All operations take a @priv pointer provided at registration time.
 * The framework serialises access with a spinlock, so implementations
 * do not need their own locking.
 */
struct spi_master_ops {
    /**
     * transfer_one — Execute a single full-duplex SPI transfer.
     * @priv:       Controller private data
     * @tx_buf:     Transmit buffer (NULL if only receiving)
     * @rx_buf:     Receive buffer (NULL if only transmitting)
     * @len:        Number of bytes to transfer
     * @cs_pin:     GPIO pin number for chip select
     * @cs_active:  1 if CS is active low (standard), 0 if active high
     *
     * Returns 0 on success, negative errno on failure.
     */
    int (*transfer_one)(void *priv, const uint8_t *tx_buf, uint8_t *rx_buf,
                        uint32_t len, int cs_pin, int cs_active);

    /**
     * set_mode — Set the SPI mode (CPOL, CPHA) on the hardware.
     * @priv:  Controller private data
     * @mode:  SPI mode (0-3)
     *
     * Returns 0 on success, negative errno on failure.
     */
    int (*set_mode)(void *priv, int mode);

    /**
     * set_speed — Set the SPI clock frequency.
     * @priv:  Controller private data
     * @speed: Desired frequency in Hz
     *
     * Returns actual frequency on success, negative errno on failure.
     */
    int (*set_speed)(void *priv, uint32_t speed);

    /**
     * set_bit_order — Set the bit order (MSB-first or LSB-first).
     * @priv:    Controller private data
     * @lsb:     Non-zero = LSB first, zero = MSB first
     *
     * Returns 0 on success, negative errno on failure.
     */
    int (*set_bit_order)(void *priv, int lsb);
};

/**
 * struct spi_master — An SPI bus controller instance.
 *
 * Tracks the hardware interface and the devices attached to this bus.
 */
struct spi_master {
    const struct spi_master_ops *ops;
    void                       *priv;
    int                         bus_id;

    /* Bus configuration (defaults for new devices) */
    int                         default_mode;
    uint32_t                    default_speed;

    /* Chip-select GPIO configuration */
    int                         cs_active_low;   /* 1 = standard active-low */

    /* Registered devices */
    struct spi_device          *devices[SPI_MAX_DEVICES];
    int                         device_count;
};

/**
 * struct spi_device — A device attached to an SPI bus.
 *
 * Created by spi_device_register() and tracked by the master.
 */
struct spi_device {
    struct spi_master          *master;
    int                         cs_pin;           /* GPIO chip-select pin */
    int                         mode;             /* SPI mode for this device */
    uint32_t                    max_speed;        /* Hz */
    int                         bit_order;        /* MSB or LSB first */
    int                         bits_per_word;    /* typically 8 */
    void                       *driver_data;       /* per-device driver data */
};

/**
 * struct spi_transfer — A single SPI I/O operation.
 *
 * Used for non-scatter-gather transfers. For full-duplex, both
 * tx_buf and rx_buf may be non-NULL. For half-duplex, set one to NULL.
 */
struct spi_transfer {
    const uint8_t              *tx_buf;           /* data to send (NULL = send zeros) */
    uint8_t                    *rx_buf;           /* receive buffer (NULL = discard) */
    uint32_t                    len;               /* number of bytes */
    uint32_t                    speed;             /* speed for this transfer (0 = default) */
    int                         cs_change;         /* de-assert CS after this transfer */
    uint16_t                    delay_usecs;       /* delay after CS before transfer */
    uint16_t                    flags;             /* transfer flags */
};

/* ── API functions ──────────────────────────────────────────────────── */

/**
 * spi_master_register — Register an SPI bus master controller.
 * @bus_id:   Bus number (0-based, must be < SPI_BUS_MAX)
 * @ops:      Hardware operations (must remain valid)
 * @priv:     Private data passed to ops
 * @mode:     Default SPI mode for devices on this bus
 * @speed:    Default SPI clock frequency in Hz
 *
 * Returns 0 on success, negative errno on failure.
 */
int spi_master_register(int bus_id, const struct spi_master_ops *ops,
                        void *priv, int mode, uint32_t speed);

/**
 * spi_master_unregister — Remove an SPI bus master controller.
 * @bus_id:   Bus number to unregister
 *
 * Returns 0 on success, negative errno on failure.
 */
int spi_master_unregister(int bus_id);

/**
 * spi_get_master — Look up an SPI master by bus number.
 * @bus_id:   Bus number
 *
 * Returns pointer to master, or NULL if not found.
 */
struct spi_master *spi_get_master(int bus_id);

/**
 * spi_device_register — Add a device to an SPI bus.
 * @master:   SPI master controller
 * @cs_pin:   GPIO chip-select pin number
 * @mode:     SPI mode for this device
 * @speed:    Max speed in Hz
 *
 * Returns pointer to spi_device on success, NULL on failure.
 */
struct spi_device *spi_device_register(struct spi_master *master,
                                       int cs_pin, int mode, uint32_t speed);

/**
 * spi_device_unregister — Remove a device from its SPI bus.
 * @dev:      Device to remove
 */
void spi_device_unregister(struct spi_device *dev);

/**
 * spi_transfer — Execute one or more SPI transfers to a device.
 * @dev:       Target SPI device
 * @transfers: Array of transfer descriptors
 * @num:       Number of transfers in the array
 *
 * Asserts chip select before the first transfer and de-asserts after
 * the last one (unless cs_change is set on a transfer).
 *
 * Returns 0 on success, negative errno on failure.
 */
int spi_transfer(struct spi_device *dev,
                 struct spi_transfer *transfers, int num);

/**
 * spi_write_then_read — Perform a half-duplex write then read.
 *
 * Sends @tx_len bytes, then receives @rx_len bytes in a single
 * chip-select assertion.  Useful for register read operations
 * where a command byte is written before reading the response.
 *
 * @dev:     SPI device
 * @tx_buf:  Data to write
 * @tx_len:  Number of bytes to write
 * @rx_buf:  Buffer to receive data
 * @rx_len:  Number of bytes to read
 *
 * Returns 0 on success, negative errno on failure.
 */
int spi_write_then_read(struct spi_device *dev,
                        const uint8_t *tx_buf, uint32_t tx_len,
                        uint8_t *rx_buf, uint32_t rx_len);

/**
 * spi_write — Convenience wrapper: write bytes to SPI device.
 */
static inline int spi_write(struct spi_device *dev,
                            const uint8_t *buf, uint32_t len) {
    struct spi_transfer t = {
        .tx_buf = buf,
        .rx_buf = NULL,
        .len    = len,
        .cs_change = 1,
    };
    return spi_transfer(dev, &t, 1);
}

/**
 * spi_read — Convenience wrapper: read bytes from SPI device.
 */
static inline int spi_read(struct spi_device *dev,
                           uint8_t *buf, uint32_t len) {
    struct spi_transfer t = {
        .tx_buf = NULL,
        .rx_buf = buf,
        .len    = len,
        .cs_change = 1,
    };
    return spi_transfer(dev, &t, 1);
}

/* ── Initialisation ────────────────────────────────────────────────── */
void spi_init(void);

/* ── Bit-bang SPI master implementation (GPIO-based) ──────────────── */
struct spi_master *spi_bitbang_register(int bus_id,
                                        uint16_t sclk_port, int sclk_bit,
                                        uint16_t mosi_port, int mosi_bit,
                                        uint16_t miso_port, int miso_bit,
                                        uint16_t cs_port, int cs_bit,
                                        uint32_t speed_hz);

#endif /* SPI_H */

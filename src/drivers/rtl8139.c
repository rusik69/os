#include "rtl8139.h"
#include "pci.h"
#include "io.h"
#include "vmm.h"
#include "string.h"
#include "printf.h"
#include "idt.h"
#include "pic.h"
#include "apic.h"
#include "net.h"
#include "netdevice.h"
#include "err.h"
#ifdef MODULE
#include "module.h"
#endif

/* ── Static driver state ─────────────────────────────────────────── */
static struct rtl8139_priv rtl8139_state;

/* ── PIO register access helpers ─────────────────────────────────── */

uint8_t rtl8139_readb(struct rtl8139_priv *priv, uint16_t reg)
{
    return inb(priv->iobase + reg);
}

uint16_t rtl8139_readw(struct rtl8139_priv *priv, uint16_t reg)
{
    return inw(priv->iobase + reg);
}

uint32_t rtl8139_readl(struct rtl8139_priv *priv, uint16_t reg)
{
    return inl(priv->iobase + reg);
}

void rtl8139_writeb(struct rtl8139_priv *priv, uint16_t reg, uint8_t val)
{
    outb(priv->iobase + reg, val);
}

void rtl8139_writew(struct rtl8139_priv *priv, uint16_t reg, uint16_t val)
{
    outw(priv->iobase + reg, val);
}

void rtl8139_writel(struct rtl8139_priv *priv, uint16_t reg, uint32_t val)
{
    outl(priv->iobase + reg, val);
}

/* ── MAC address access ──────────────────────────────────────────── */

void rtl8139_get_mac(struct rtl8139_priv *priv, uint8_t *mac)
{
    mac[0] = rtl8139_readb(priv, RTL_REG_IDR0);
    mac[1] = rtl8139_readb(priv, RTL_REG_IDR1);
    mac[2] = rtl8139_readb(priv, RTL_REG_IDR2);
    mac[3] = rtl8139_readb(priv, RTL_REG_IDR3);
    mac[4] = rtl8139_readb(priv, RTL_REG_IDR4);
    mac[5] = rtl8139_readb(priv, RTL_REG_IDR5);
}

int rtl8139_set_mac(struct rtl8139_priv *priv, const uint8_t *mac)
{
    /* MAC address registers are writable only when the EEPROM is in
     * normal operating mode (9346CR bits EEM0|EEM1 both set). */
    uint8_t eeprom_mode = rtl8139_readb(priv, RTL_REG_9346CR);
    if ((eeprom_mode & 0x03) != RTL_9346CR_EECLK) {
        /* Not in normal mode — write to 9346CR to enter normal op mode
         * (EEM0|EEM1 = 0x03, which is the "normal operation" state). */
        rtl8139_writeb(priv, RTL_REG_9346CR, 0x03);
    }

    rtl8139_writeb(priv, RTL_REG_IDR0, mac[0]);
    rtl8139_writeb(priv, RTL_REG_IDR1, mac[1]);
    rtl8139_writeb(priv, RTL_REG_IDR2, mac[2]);
    rtl8139_writeb(priv, RTL_REG_IDR3, mac[3]);
    rtl8139_writeb(priv, RTL_REG_IDR4, mac[4]);
    rtl8139_writeb(priv, RTL_REG_IDR5, mac[5]);

    /* Restore EEPROM mode */
    rtl8139_writeb(priv, RTL_REG_9346CR, eeprom_mode);
    return 0;
}

/* ── Hardware reset ──────────────────────────────────────────────── */

int rtl8139_reset(struct rtl8139_priv *priv)
{
    int timeout;

    /* Issue software reset via CR register */
    rtl8139_writeb(priv, RTL_REG_CR, RTL_CR_RST);

    /* Wait for reset to complete (RST bit self-clears) */
    timeout = 1000000;
    while ((rtl8139_readb(priv, RTL_REG_CR) & RTL_CR_RST) && timeout > 0) {
        io_wait();
        timeout--;
    }

    if (timeout == 0) {
        kprintf("  rtl8139: reset timed out\n");
        return -ETIMEDOUT;
    }

    kprintf("  rtl8139: reset complete\n");
    return 0;
}

/* ── Hardware initialization ─────────────────────────────────────── */

int rtl8139_init_hw(struct rtl8139_priv *priv)
{
    uint32_t tcr, rcr;
    int ret;

    /* Step 1: Reset the chip */
    ret = rtl8139_reset(priv);
    if (ret < 0)
        return ret;

    /* Step 2: Read MAC address from on-chip registers */
    rtl8139_get_mac(priv, priv->mac);
    kprintf("  rtl8139: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            priv->mac[0], priv->mac[1], priv->mac[2],
            priv->mac[3], priv->mac[4], priv->mac[5]);

    /* Step 3: Unlock configuration registers (9346CR to normal mode) */
    rtl8139_writeb(priv, RTL_REG_9346CR, 0x03);  /* EEM0|EEM1 = normal */

    /* Step 4: Set CONFIG1 — turn off optional features */
    rtl8139_writeb(priv, RTL_REG_CONFIG1,
                   rtl8139_readb(priv, RTL_REG_CONFIG1) & ~RTL_CFG1_PMEN);

    /* Step 5: Configure Transmit Configuration Register (TCR) */
    tcr = rtl8139_readl(priv, RTL_REG_TCR);
    tcr &= ~(0x0F << 8);          /* Clear MXDMA field */
    tcr |= RTL_TCR_MXDMA_256;     /* 256-dword max DMA burst */
    tcr &= ~RTL_TCR_IFG2;         /* Clear IFG2 */
    rtl8139_writel(priv, RTL_REG_TCR, tcr);

    /* Step 6: Configure Receive Configuration Register (RCR) */
    rcr = RTL_RCR_DEFAULT;
    rtl8139_writel(priv, RTL_REG_RCR, rcr);

    /* Step 7: Clear missed packet counter */
    rtl8139_readl(priv, RTL_REG_MPC);

    /* Step 8: Lock configuration registers */
    rtl8139_writeb(priv, RTL_REG_9346CR, RTL_9346CR_EECLK);

    /* Step 9: Enable RX and TX */
    rtl8139_writeb(priv, RTL_REG_CR, RTL_CR_TE | RTL_CR_RE);

    kprintf("  rtl8139: hardware initialized\n");
    priv->initialized = 1;
    return 0;
}

/* ── Shutdown ────────────────────────────────────────────────────── */

void rtl8139_shutdown(struct rtl8139_priv *priv)
{
    /* Disable RX and TX */
    rtl8139_writeb(priv, RTL_REG_CR, RTL_CR_RST);

    /* Mask all interrupts */
    rtl8139_writew(priv, RTL_REG_IMR, 0);

    /* Acknowledge any pending interrupt */
    rtl8139_readw(priv, RTL_REG_ISR);

    priv->initialized = 0;
    kprintf("  rtl8139: shutdown complete\n");
}

/* ── PCI probe ───────────────────────────────────────────────────── */

int rtl8139_find_device(struct pci_device *pci_dev)
{
    int ret;

    ret = pci_find_device(RTL8139_VENDOR, RTL8139_DEVICE, pci_dev);
    if (ret < 0)
        return -ENODEV;

    return 0;
}

int rtl8139_probe(struct rtl8139_priv *priv)
{
    struct pci_device pci_dev;
    uint16_t iobase;
    int ret;

    /* Find the RTL8139 on PCI bus */
    ret = rtl8139_find_device(&pci_dev);
    if (ret < 0) {
        kprintf("  rtl8139: device not found\n");
        return ret;
    }

    /* BAR0 contains the I/O port base address.
     * Mask off the lower bits (1 = I/O space indicator). */
    iobase = (uint16_t)(pci_dev.bar[0] & 0xFFFC);
    if (iobase == 0) {
        kprintf("  rtl8139: invalid PIO base from BAR0\n");
        return -ENODEV;
    }

    priv->iobase  = iobase;
    priv->irq_line = pci_dev.irq;
    priv->initialized = 0;
    priv->tx_cur = 0;

    /* Enable PCI bus mastering */
    pci_enable_bus_master(&pci_dev);

    kprintf("  rtl8139: found at I/O base 0x%04x, IRQ %d\n",
            priv->iobase, priv->irq_line);

    return 0;
}

/* ── Module / device init entry point ────────────────────────────── */

int rtl8139_init(void)
{
    int ret;

    memset(&rtl8139_state, 0, sizeof(rtl8139_state));

    ret = rtl8139_probe(&rtl8139_state);
    if (ret < 0)
        return ret;

    ret = rtl8139_init_hw(&rtl8139_state);
    if (ret < 0) {
        kprintf("  rtl8139: hardware init failed\n");
        return ret;
    }

    kprintf("rtl8139: driver loaded\n");
    return 0;
}

void rtl8139_exit(void)
{
    if (rtl8139_state.initialized)
        rtl8139_shutdown(&rtl8139_state);
    kprintf("rtl8139: driver unloaded\n");
}

#ifdef MODULE
module_init(rtl8139_init);
module_exit(rtl8139_exit);
MODULE_LICENSE("GPL");
MODULE_VERSION("1.0");
MODULE_DESCRIPTION("Realtek RTL8139 Fast Ethernet driver (PIO register access)");
MODULE_AUTHOR("1000 Changes Project");
MODULE_ALIAS("pci:v000010ECd00008139*");
#endif

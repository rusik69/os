#include "floppy.h"
#include "printf.h"
#include "io.h"
#include "string.h"
#include "timer.h"
#include "idt.h"
#include "heap.h"
#include "dma.h"
#include "pmm.h"

/*
 * floppy.c — Floppy disk controller driver
 *
 * Supports dual build as a loadable kernel module (.ko) or compiled
 * directly into the kernel.  When MODULE is defined the module loader
 * calls init_module() / cleanup_module(); otherwise the built-in path
 * calls floppy_init() directly.
 *
 * Implements FDC (NEC µPD765 / Intel 8272A) detection and operation:
 *   - Probe FDC primary (0x3F0-0x3F7) and secondary (0x370-0x377) I/O ports
 *   - Controller reset, sense interrupt, recalibrate, seek, read
 *   - DMA-based read for floppy_read_sectors
 */

#ifdef MODULE
#include "module.h"
#endif

/* ── FDC register map (primary base 0x3F0, secondary base 0x370) ──── */
#define FDC_DOR    2   /* Digital Output Register */
#define FDC_MSR    4   /* Main Status Register */
#define FDC_FIFO   5   /* Data FIFO (R/W) */
#define FDC_DIR    7   /* Digital Input Register (read) / CCR (write) */

/* DOR (Digital Output Register) bits */
#define DOR_MOTD   (1u << 7)  /* Motor D */
#define DOR_MOTC   (1u << 6)  /* Motor C */
#define DOR_MOTB   (1u << 5)  /* Motor B */
#define DOR_MOTA   (1u << 4)  /* Motor A */
#define DOR_IRQ    (1u << 3)  /* Enable IRQ from FDC */
#define DOR_RESET  (1u << 2)  /* Reset FDC (0 = reset, 1 = enabled) */
#define DOR_DSEL1  (1u << 1)  /* Drive select bit 1 */
#define DOR_DSEL0  (1u << 0)  /* Drive select bit 0 */

/* MSR (Main Status Register) bits */
#define MSR_RQM    (1u << 7)  /* Request for Master */
#define MSR_DIO    (1u << 6)  /* Data I/O direction (1 = read, 0 = write) */
#define MSR_NDM    (1u << 5)  /* Non-DMA mode */
#define MSR_CB     (1u << 4)  /* Command busy */
#define MSR_ACTD   (1u << 3)  /* Drive D active */
#define MSR_ACTC   (1u << 2)  /* Drive C active */
#define MSR_ACTB   (1u << 1)  /* Drive B active */
#define MSR_ACTA   (1u << 0)  /* Drive A active */

/* FIFO commands */
#define FDC_CMD_SPECIFY          0x03
#define FDC_CMD_SENSE_INTERRUPT  0x08
#define FDC_CMD_RECALIBRATE      0x07
#define FDC_CMD_SEEK             0x0F
#define FDC_CMD_READ_DATA        0xE6  /* MFM + implied seek + DMA */
#define FDC_CMD_READ_ID          0x4A

/* ── Driver state ────────────────────────────────────────────────── */
static uint16_t g_fdc_base = 0;    /* I/O base address of FDC */
static int      g_fdc_irq  = 6;    /* IRQ 6 for FDC */
static int      g_floppy_present = 0;
static int      g_floppy_irq_received = 0;
static int      g_floppy_drive_type = 0;  /* 0=unknown, 1=360K, ... */
static int      g_floppy_motor_on = 0;

/* ── IRQ handling ─────────────────────────────────────────────────── */
static void floppy_irq_handler(struct interrupt_frame *frame)
{
    (void)frame;
    g_floppy_irq_received = 1;
}

/* ── FDC I/O helpers ─────────────────────────────────────────────── */

/* Wait until the FIFO is ready for a data byte transfer (RQM=1) */
static int fdc_wait_rqm(void)
{
    int timeout = 100000;
    while (timeout--) {
        uint8_t msr = inb(g_fdc_base + FDC_MSR);
        if (msr & MSR_RQM)
            return 0;
        io_wait();
    }
    return -1;  /* timeout */
}

/* Send a command byte to the FDC FIFO */
static int fdc_send_byte(uint8_t val)
{
    if (fdc_wait_rqm() < 0)
        return -1;
    outb(g_fdc_base + FDC_FIFO, val);
    return 0;
}

/* Read a result byte from the FDC FIFO */
static int fdc_recv_byte(uint8_t *val)
{
    if (fdc_wait_rqm() < 0)
        return -1;
    uint8_t msr = inb(g_fdc_base + FDC_MSR);
    if (!(msr & MSR_DIO))
        return -1;  /* not in read mode */
    *val = inb(g_fdc_base + FDC_FIFO);
    return 0;
}

/* Send command bytes to FDC */
static int fdc_send_command(const uint8_t *cmd, int len)
{
    for (int i = 0; i < len; i++) {
        if (fdc_send_byte(cmd[i]) < 0)
            return -1;
    }
    return 0;
}

/* Receive result bytes from FDC */
static int fdc_recv_result(uint8_t *buf, int max_len)
{
    int count = 0;
    while (count < max_len) {
        /* Check if FDC has data for us (CB cleared means no more data) */
        uint8_t msr = inb(g_fdc_base + FDC_MSR);
        if (!(msr & MSR_CB))
            break;
        if (fdc_recv_byte(&buf[count]) < 0)
            break;
        count++;
    }
    return count;
}

/* ── FDC reset sequence ──────────────────────────────────────────── */

static int fdc_reset(void)
{
    /* Reset the FDC by clearing the RESET bit in DOR */
    outb(g_fdc_base + FDC_DOR, 0x00);
    io_wait();
    timer_udelay(100);
    /* Re-enable the FDC with IRQ enabled */
    outb(g_fdc_base + FDC_DOR, DOR_IRQ);
    io_wait();
    timer_udelay(100);

    /* Wait for sense interrupt after reset (up to 4 drives) */
    for (int i = 0; i < 4; i++) {
        g_floppy_irq_received = 0;
        /* Wait for IRQ or timeout */
        int timeout = 100000;
        while (timeout--) {
            if (g_floppy_irq_received)
                break;
            io_wait();
        }

        /* Send SENSE INTERRUPT */
        if (fdc_send_command((uint8_t[]){FDC_CMD_SENSE_INTERRUPT}, 1) < 0)
            return -1;

        uint8_t st0, pcn;
        if (fdc_recv_byte(&st0) < 0 || fdc_recv_byte(&pcn) < 0)
            return -1;

        kprintf("[FLOPPY] Reset sense: drive %d, ST0=0x%02x, PCN=%d\n",
                i, st0, pcn);
    }

    /* Configure FDC: SPECIFY command (SPP rate, SRT = 0x0C, HUT = 0x0F, HLT = 0x02) */
    uint8_t specify_cmd[] = {FDC_CMD_SPECIFY, 0x0C, 0x02};
    if (fdc_send_command(specify_cmd, 3) < 0)
        return -1;

    kprintf("[FLOPPY] Controller reset OK\n");
    return 0;
}

/* ── Floppy detection ────────────────────────────────────────────── */

static int floppy_probe_fdc(uint16_t base)
{
    /* Try to reset the FDC at this base address */
    g_fdc_base = base;

    /* Check if the MSR register responds by reading it */
    uint8_t msr = inb(base + FDC_MSR);

    /* For a valid FDC, MSR should have some expected bits.
     * After power-on, RQM should be set. We also check that
     * the register is not just reading back 0xFF (no device) or 0x00 (stuck). */
    if (msr == 0xFF || msr == 0x00) {
        g_fdc_base = 0;
        return -1;
    }

    return 0;
}

/* ── Drive operations ────────────────────────────────────────────── */

/* Select a drive and turn on its motor */
static void floppy_select_drive(int drive)
{
    uint8_t dor = DOR_IRQ;  /* Keep IRQ enabled */
    /* Motor bits: A=bit4, B=bit5, C=bit6, D=bit7 */
    if (drive == 0)
        dor |= DOR_MOTA;
    else if (drive == 1)
        dor |= DOR_MOTB;
    /* Drive select bits */
    dor |= (uint8_t)(drive & 0x03);
    outb(g_fdc_base + FDC_DOR, dor);
    g_floppy_motor_on = 1;
    /* Give the motor time to spin up (~250ms) */
    timer_udelay(250000);
}

/* Turn off motor for the given drive */
static void floppy_deselect_drive(int drive)
{
    uint8_t dor = DOR_IRQ;
    if (drive == 0)
        dor &= ~DOR_MOTA;
    else if (drive == 1)
        dor &= ~DOR_MOTB;
    dor |= (uint8_t)(drive & 0x03);
    outb(g_fdc_base + FDC_DOR, dor);
    g_floppy_motor_on = 0;
}

/* Recalibrate (seek to track 0) */
static int floppy_recalibrate(int drive)
{
    floppy_select_drive(drive);
    g_floppy_irq_received = 0;

    uint8_t cmd[] = {FDC_CMD_RECALIBRATE, (uint8_t)(drive & 0x03)};
    if (fdc_send_command(cmd, 2) < 0) {
        floppy_deselect_drive(drive);
        return -1;
    }

    /* Wait for IRQ */
    int timeout = 500000;
    while (timeout--) {
        if (g_floppy_irq_received)
            break;
        io_wait();
    }
    if (!g_floppy_irq_received) {
        kprintf("[FLOPPY] Recalibrate timeout\n");
        floppy_deselect_drive(drive);
        return -1;
    }

    /* Sense interrupt */
    uint8_t st0, pcn;
    if (fdc_send_command((uint8_t[]){FDC_CMD_SENSE_INTERRUPT}, 1) < 0 ||
        fdc_recv_byte(&st0) < 0 || fdc_recv_byte(&pcn) < 0) {
        floppy_deselect_drive(drive);
        return -1;
    }

    if (st0 & 0x20) {
        kprintf("[FLOPPY] Recalibrate failed (ST0=0x%02x, PCN=%d)\n", st0, pcn);
        floppy_deselect_drive(drive);
        return -1;
    }

    kprintf("[FLOPPY] Drive %d recalibrated to track %d\n", drive, pcn);
    floppy_deselect_drive(drive);
    return 0;
}

/* Seek to a specific cylinder (track) */
static int floppy_seek(int drive, int cylinder)
{
    floppy_select_drive(drive);
    g_floppy_irq_received = 0;

    uint8_t cmd[] = {FDC_CMD_SEEK, (uint8_t)(drive & 0x03), (uint8_t)cylinder};
    if (fdc_send_command(cmd, 3) < 0) {
        floppy_deselect_drive(drive);
        return -1;
    }

    /* Wait for IRQ */
    int timeout = 500000;
    while (timeout--) {
        if (g_floppy_irq_received)
            break;
        io_wait();
    }
    if (!g_floppy_irq_received) {
        kprintf("[FLOPPY] Seek timeout (cyl %d)\n", cylinder);
        floppy_deselect_drive(drive);
        return -1;
    }

    /* Sense interrupt */
    uint8_t st0, pcn;
    if (fdc_send_command((uint8_t[]){FDC_CMD_SENSE_INTERRUPT}, 1) < 0 ||
        fdc_recv_byte(&st0) < 0 || fdc_recv_byte(&pcn) < 0) {
        floppy_deselect_drive(drive);
        return -1;
    }

    if (st0 & 0x20) {
        kprintf("[FLOPPY] Seek failed (ST0=0x%02x, PCN=%d)\n", st0, pcn);
        floppy_deselect_drive(drive);
        return -1;
    }

    floppy_deselect_drive(drive);
    return 0;
}

/* ── DMA setup for floppy reads ─────────────────────────────────── */

/* The PC floppy controller uses DMA channel 2 for data transfers.
 * We set up ISA DMA (8237A) in auto-initialize mode, single transfer,
 * write transfer (memory ← I/O), 16-bit. */

#define FLOPPY_DMA_CHANNEL 2

/* Program the DMA controller for a floppy read (memory ← FDC).
 * @buf_phys: physical address of the buffer (must be < 16 MB for ISA DMA)
 * @len: transfer length in bytes (must be a multiple of 128)
 */
static int floppy_setup_dma_read(uint32_t buf_phys, uint32_t len)
{
    /* DMA channel 2 is used by floppy.
     * Registers for channel 2:
     *   Base address:   DMA+4  (0x04)
     *   Base count:     DMA+5  (0x05)
     *   Page register:  0x81
     *   Mode register:  DMA+0xB (0x0B) — write mode, then DMA+0x0A (0x0A) to mask
     *
     * DMA controller base is 0x00 for the first 8237A (channels 0-3)
     */

    /* Mask DMA channel 2 */
    outb(0x0A, 0x04 | FLOPPY_DMA_CHANNEL);  /* 0x0A = Mask register, bit 2 = channel 2 */

    /* Clear flip-flop (send byte pointer to low byte) */
    outb(0x0C, 0x00);

    /* Base address: low byte then high byte */
    outb(0x04, (uint8_t)(buf_phys & 0xFF));
    outb(0x04, (uint8_t)((buf_phys >> 8) & 0xFF));

    /* Base count: number of bytes - 1 */
    uint32_t count = len - 1;
    outb(0x05, (uint8_t)(count & 0xFF));
    outb(0x05, (uint8_t)((count >> 8) & 0xFF));

    /* Page register for channel 2 (0x81) */
    outb(0x81, (uint8_t)((buf_phys >> 16) & 0xFF));

    /* Clear flip-flop again */
    outb(0x0C, 0x00);

    /* Mode: channel 2, read transfer (memory ← I/O), auto-init, single */
    /* 0x56 = 01 01 0110
     *   Mode register format: bits 7-6 (mode), bit 5 (auto-init), bit 4 (direction),
     *   bits 3-2 (channel select), bits 1-0 (transfer type)
     *   01 = single transfer mode
     *   0 = auto-init disabled
     *   1 = read transfer (I/O → memory)
     *   10 = channel 2
     *   10 = write transfer type (actually 01 = read for 8237A)
     * Actually: 0x46 = read transfer (mem ← I/O), channel 2, single
     *   0x46 = 01 00 01 10
     *   bits 7-6: 01 = single transfer mode
     *   bit 5: 0 = no auto-init
     *   bit 4: 0 = read (memory ← I/O)
     *   bits 3-2: 01 = channel 2
     *   bits 1-0: 10 = demand/block? No, 01 for write, 10 for read? 
     *   Actually for 8237A: 00 = verify, 01 = write (mem → I/O), 10 = read (I/O → mem)
     */
    outb(0x0B, 0x46);  /* mode: channel 2, single, read, no auto-init */

    /* Unmask DMA channel 2 */
    outb(0x0A, FLOPPY_DMA_CHANNEL);  /* unmask channel 2 */

    return 0;
}

/* ── Read sectors ────────────────────────────────────────────────── */

/* CHS geometry for a standard 1.44 MB floppy (3.5" HD):
 *   80 cylinders, 2 heads, 18 sectors/track
 *   512 bytes/sector
 */
#define FLOPPY_CYLINDERS  80
#define FLOPPY_HEADS      2
#define FLOPPY_SECTORS    18
#define FLOPPY_SECTOR_SIZE 512

int floppy_read_sectors(int drive, uint32_t lba, uint8_t count, void *buf)
{
    if (drive < 0 || drive > 3 || !buf || count == 0)
        return -1;
    if (!g_floppy_present)
        return -1;

    /* Convert LBA to CHS */
    int cylinder = (int)(lba / (FLOPPY_HEADS * FLOPPY_SECTORS));
    int head    = (int)((lba / FLOPPY_SECTORS) % FLOPPY_HEADS);
    int sector  = (int)(lba % FLOPPY_SECTORS) + 1;  /* sectors are 1-based */
    int total_bytes = count * FLOPPY_SECTOR_SIZE;

    if (cylinder >= FLOPPY_CYLINDERS)
        return -1;

    kprintf("[FLOPPY] Read drive %d: LBA=%u -> CHS=%d/%d/%d count=%d\n",
            drive, lba, cylinder, head, sector, count);

    /* Allocate DMA buffer below 16 MB (ISA DMA limitation).
     * Use kmalloc which should give us low memory on most x86 systems. */
    void *dma_buf = kmalloc(total_bytes);
    if (!dma_buf)
        return -1;
    memset(dma_buf, 0, total_bytes);

    uintptr_t dma_phys = (uintptr_t)VIRT_TO_PHYS(dma_buf);
    if (dma_phys >= 0x1000000ULL) {
        /* Buffer too high for ISA DMA — this won't work on real hardware
         * but on QEMU/firmware it might be OK. Try anyway. */
        kprintf("[FLOPPY] WARNING: DMA buffer at 0x%lx may be too high for ISA DMA\n",
                (unsigned long)dma_phys);
    }

    floppy_select_drive(drive);

    /* Seek to the right cylinder */
    floppy_seek(drive, cylinder);

    /* Set up DMA */
    floppy_setup_dma_read((uint32_t)dma_phys, (uint32_t)total_bytes);

    /* Wait for DMA controller to be ready */
    timer_udelay(100);

    /* Send READ DATA command:
     *   Byte 0: Command (0xE6 = MFM + skip + implied seek + DMA)
     *   Byte 1: Head/Drive (bits 0-1 = drive, bit 2 = head)
     *   Byte 2: Cylinder (C)
     *   Byte 3: Head (H)
     *   Byte 4: Sector (R)
     *   Byte 5: Sector size code (2 = 512 bytes/sector)
     *   Byte 6: End of track (last sector on current track)
     *   Byte 7: Gap length (0x1B = default for 3.5")
     *   Byte 8: Data length (0xFF = unused for sector size != 0)
     */
    uint8_t cmd[9];
    cmd[0] = 0xE6;               /* READ DATA with MFM, implied seek, DMA */
    cmd[1] = (uint8_t)((head & 1) << 2) | (uint8_t)(drive & 0x03);
    cmd[2] = (uint8_t)cylinder;  /* C (cylinder) */
    cmd[3] = (uint8_t)head;      /* H (head) */
    cmd[4] = (uint8_t)sector;    /* R (sector) */
    cmd[5] = 2;                  /* N (sector size code: 2 = 512 bytes) */
    cmd[6] = FLOPPY_SECTORS;     /* EOT (end of track) */
    cmd[7] = 0x1B;               /* GPL (gap length) */
    cmd[8] = 0xFF;               /* DTL (data length, unused) */

    g_floppy_irq_received = 0;

    if (fdc_send_command(cmd, 9) < 0) {
        kprintf("[FLOPPY] Read command failed\n");
        floppy_deselect_drive(drive);
        kfree(dma_buf);
        return -1;
    }

    /* Wait for IRQ (DMA transfer complete) */
    int timeout = 500000;
    while (timeout--) {
        if (g_floppy_irq_received)
            break;
        io_wait();
    }

    /* Read result bytes from FDC */
    uint8_t st[7];
    int res_count = fdc_recv_result(st, 7);

    floppy_deselect_drive(drive);

    if (!g_floppy_irq_received) {
        kprintf("[FLOPPY] Read timeout\n");
        kfree(dma_buf);
        return -1;
    }

    /* Check result for errors */
    if (res_count >= 1 && (st[0] & 0xC0)) {
        kprintf("[FLOPPY] Read error: ST0=0x%02x ST1=0x%02x ST2=0x%02x "
                "C=%d H=%d R=%d N=%d\n",
                st[0], st[1], st[2], st[3], st[4], st[5], st[6]);
        kfree(dma_buf);
        return -1;
    }

    if (res_count >= 1)
        kprintf("[FLOPPY] Read OK: ST0=0x%02x C=%d H=%d R=%d N=%d (%d bytes)\n",
                st[0], st[3], st[4], st[5], st[6], total_bytes);

    /* Copy DMA buffer to caller's buffer */
    memcpy(buf, dma_buf, (size_t)total_bytes);
    kfree(dma_buf);

    return 0;
}

/* ── Driver API ──────────────────────────────────────────────────── */

int floppy_is_present(void)
{
    return g_floppy_present;
}

/* ── Initialisation ──────────────────────────────────────────────── */

int floppy_init(void)
{
    /* Register IRQ handler for FDC (IRQ 6 -> vector 32+6=38) */
    idt_register_handler((uint8_t)(IRQ_OFFSET + g_fdc_irq), floppy_irq_handler);

    /* Probe primary FDC base (0x3F0) */
    if (floppy_probe_fdc(0x3F0) == 0) {
        kprintf("[FLOPPY] FDC found at primary base 0x3F0\n");
    }
    /* Probe secondary FDC base (0x370) if primary not found */
    else if (floppy_probe_fdc(0x370) == 0) {
        kprintf("[FLOPPY] FDC found at secondary base 0x370\n");
    } else {
        kprintf("[--] Floppy: no controller found\n");
        /* IRQ handler registered; since there's no irq_unregister_handler
         * API, we leave it registered but harmless. */
        return -1;
    }

    /* Reset the FDC controller */
    if (fdc_reset() < 0) {
        kprintf("[--] Floppy: FDC reset failed\n");
        g_fdc_base = 0;
        return -1;
    }

    /* Recalibrate drive 0 (A:) to see if it's present */
    if (floppy_recalibrate(0) == 0) {
        g_floppy_present = 1;
        g_floppy_drive_type = 1;  /* Assume 1.44 MB */
        kprintf("[OK] Floppy drive 0 detected and recalibrated\n");
    } else {
        kprintf("[--] Floppy: drive 0 not present or failed recalibrate\n");
    }

    if (g_floppy_present) {
        kprintf("[OK] Floppy: controller at 0x%x, drive 0 present\n",
                (unsigned)g_fdc_base);
        return 0;
    }

    /* No drive found but controller might be present */
    return -1;
}

/* ── Module hotplug (loadable module path) ───────────────────────── */

#ifdef MODULE

/* Module entry point — called by the ELF module loader on insmod. */
int init_module(void)
{
    return floppy_init();
}

/* Module exit point — called by the ELF module loader on rmmod. */
void cleanup_module(void)
{
    g_fdc_base = 0;
    g_floppy_present = 0;
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hermes OS Kernel Team");
MODULE_DESCRIPTION("Floppy disk controller driver");

#endif /* MODULE */

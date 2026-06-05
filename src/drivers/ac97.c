/*
 * AC'97 audio driver  (PCI class 04:01)
 *
 * Supports Intel 82801AA (ICH) and compatible AC'97 audio controllers.
 * QEMU emulates this device when "-soundhw ac97" or "-device AC97" is used.
 *
 * The driver uses the NABM (Native Audio Bus Master) DMA engine to stream
 * PCM samples via a Buffer Descriptor List (BDL).
 */

#include "ac97.h"
#include "pci.h"
#include "string.h"
#include "printf.h"
#include "io.h"
#include "types.h"

/* ── PCI identification ─────────────────────────────────────────── */
#define AC97_CLASS    0x04
#define AC97_SUBCLASS 0x01

/* ── Register offsets ───────────────────────────────────────────── */
/* NAM (Native Audio Mixer) — mapped to BAR0 (I/O) */
#define NAM_RESET       0x00
#define NAM_MASTER_VOL  0x02
#define NAM_PCM_VOL     0x18
#define NAM_SAMPLE_RATE 0x2C   /* PCM front DAC rate */

/* NABM (Native Audio Bus Master) — mapped to BAR1 (I/O) */
#define NABM_PCM_OUT_BDBAR  0x10  /* Buffer Descriptor Base Address Register */
#define NABM_PCM_OUT_CIV    0x14  /* Current Index Value */
#define NABM_PCM_OUT_LVI    0x15  /* Last Valid Index */
#define NABM_PCM_OUT_SR     0x16  /* Status Register */
#define NABM_PCM_OUT_CR     0x1B  /* Control Register */
#define NABM_GLOB_CNT       0x2C  /* Global Control */
#define NABM_GLOB_STS       0x30  /* Global Status */

#define CR_RPBM  (1 << 0)   /* Run/Pause Bus Master */
#define CR_RR    (1 << 1)   /* Reset Registers */
#define CR_LVBIE (1 << 2)   /* Last Valid Buffer Interrupt Enable */
#define CR_FEIE  (1 << 3)   /* FIFO Error Interrupt Enable */
#define CR_IOCE  (1 << 4)   /* IOC Interrupt Enable */

/* ── BDL entry ──────────────────────────────────────────────────── */
#pragma pack(push, 1)
struct ac97_bdl_entry {
    uint32_t addr;     /* physical address of audio buffer */
    uint16_t samples;  /* number of 16-bit samples */
    uint16_t ctrl;     /* BUP (0x4000) = Buffer Underrun Policy, IOC (0x8000) */
};
#pragma pack(pop)

#define BDL_ENTRIES  32
#define AC97_IOC     0x8000   /* Interrupt on Completion flag */

/* ── Driver state ────────────────────────────────────────────────── */
static int      ac97_dev_present = 0;
static uint16_t ac97_nam_base    = 0;
static uint16_t ac97_nabm_base   = 0;

/* BDL and audio buffers — statically allocated (4KB aligned for DMA) */
static struct ac97_bdl_entry __attribute__((aligned(4096))) bdl[BDL_ENTRIES];
static int16_t __attribute__((aligned(4096))) audio_buf[BDL_ENTRIES][4096];

/* ── Helpers ─────────────────────────────────────────────────────── */
static inline void nam_out16(uint16_t reg, uint16_t v) { outw(ac97_nam_base  + reg, v); }
static inline void nam_out32(uint16_t reg, uint32_t v) {
    outw(ac97_nam_base + reg,     (uint16_t)v);
    outw(ac97_nam_base + reg + 2, (uint16_t)(v >> 16));
}
static inline uint16_t nam_in16(uint16_t reg) { return inw(ac97_nam_base + reg); }

static inline void nabm_out8 (uint16_t reg, uint8_t  v) { outb(ac97_nabm_base + reg, v);  }
static inline void nabm_out16(uint16_t reg, uint16_t v) { outw(ac97_nabm_base + reg, v);  }
static inline void nabm_out32(uint16_t reg, uint32_t v) {
    outw(ac97_nabm_base + reg,     (uint16_t)v);
    outw(ac97_nabm_base + reg + 2, (uint16_t)(v >> 16));
}
static inline uint8_t  nabm_in8 (uint16_t reg) { return inb(ac97_nabm_base + reg);  }
static inline uint16_t nabm_in16(uint16_t reg) { return inw(ac97_nabm_base + reg);  }

/* ── Init ────────────────────────────────────────────────────────── */
int ac97_init(void) {
    struct pci_device dev;
    if (pci_find_class(AC97_CLASS, AC97_SUBCLASS, &dev) < 0)
        return -1;

    ac97_nam_base  = (uint16_t)(dev.bar[0] & ~0x3u);
    ac97_nabm_base = (uint16_t)(dev.bar[1] & ~0x3u);
    if (!ac97_nam_base || !ac97_nabm_base) return -1;

    pci_enable_bus_master(&dev);

    /* Cold reset via GLOB_CNT */
    nabm_out32(NABM_GLOB_CNT, 0x00000002);  /* assert cold reset */
    /* Wait a moment (busy loop ~10ms worth of iterations) */
    for (volatile uint32_t i = 0; i < 100000; i++);
    nabm_out32(NABM_GLOB_CNT, 0x00000000);  /* release reset */
    for (volatile uint32_t i = 0; i < 100000; i++);

    /* Reset NAM */
    nam_out16(NAM_RESET, 1);
    for (volatile uint32_t i = 0; i < 10000; i++);

    /* Set master volume to max (0x0000 = 0dB, no mute) */
    nam_out16(NAM_MASTER_VOL, 0x0000);
    nam_out16(NAM_PCM_VOL,    0x0000);

    /* Set default sample rate to 44100 Hz */
    nam_out16(NAM_SAMPLE_RATE, 44100);

    /* Reset PCM-out DMA engine */
    nabm_out8(NABM_PCM_OUT_CR, CR_RR);
    for (volatile uint32_t i = 0; i < 10000; i++);
    nabm_out8(NABM_PCM_OUT_CR, 0);

    ac97_dev_present = 1;
    kprintf("ac97: initialized (NAM=0x%lx, NABM=0x%lx)\n",
            (unsigned long)ac97_nam_base, (unsigned long)ac97_nabm_base);
    return 0;
}

/* ── Play PCM ────────────────────────────────────────────────────── */
void ac97_play_pcm(const int16_t *samples, uint32_t len, uint32_t rate) {
    if (!ac97_dev_present || !samples || len == 0) return;

    /* Set sample rate */
    if (rate != 44100) nam_out16(NAM_SAMPLE_RATE, (uint16_t)rate);

    /* Reset DMA */
    nabm_out8(NABM_PCM_OUT_CR, CR_RR);
    for (volatile uint32_t i = 0; i < 10000; i++);
    nabm_out8(NABM_PCM_OUT_CR, 0);

    /* Fill BDL */
    uint32_t remaining = len;
    uint32_t src_off   = 0;
    int      n_entries = 0;

    while (remaining > 0 && n_entries < BDL_ENTRIES) {
        uint32_t chunk = remaining;
        if (chunk > sizeof(audio_buf[0])) chunk = sizeof(audio_buf[0]);
        memcpy(audio_buf[n_entries], (const uint8_t *)samples + src_off, chunk);
        bdl[n_entries].addr    = (uint32_t)(uintptr_t)audio_buf[n_entries];
        bdl[n_entries].samples = (uint16_t)(chunk / 2); /* 16-bit samples */
        bdl[n_entries].ctrl    = (n_entries == (BDL_ENTRIES - 1) || remaining - chunk == 0)
                                 ? AC97_IOC : 0;
        src_off   += chunk;
        remaining -= chunk;
        n_entries++;
    }

    /* Program BDL address and LVI */
    nabm_out32(NABM_PCM_OUT_BDBAR, (uint32_t)(uintptr_t)bdl);
    nabm_out8 (NABM_PCM_OUT_LVI,   (uint8_t)(n_entries - 1));

    /* Start DMA */
    nabm_out8(NABM_PCM_OUT_CR, CR_RPBM | CR_IOCE);

    /* Wait until DMA completes (busy-wait; max ~30s timeout) */
    uint32_t timeout = 0xFFFFFFF;
    while (timeout--) {
        uint16_t sr = nabm_in16(NABM_PCM_OUT_SR);
        if (sr & 0x0010) break;  /* IOC: interrupt on completion */
        __asm__ volatile("pause");
    }

    /* Stop DMA and clear status */
    nabm_out8 (NABM_PCM_OUT_CR, 0);
    nabm_out16(NABM_PCM_OUT_SR, 0x001E);  /* clear all status bits */

    if (rate != 44100) nam_out16(NAM_SAMPLE_RATE, 44100);
}

/* ── Mixer control ────────────────────────────────────────────────── */

/* AC97 NAM volume register bits */
#define AC97_VOL_MASK   0x001F   /* bits 0-4: left vol, bits 8-12: right vol */
#define AC97_MUTE_BIT   0x8000   /* bit 15: mute */

void ac97_set_volume(uint16_t channel, uint8_t left, uint8_t right, int mute) {
    if (!ac97_dev_present)
        return;

    /* Clamp to valid range (0–31 per AC97 spec) */
    if (left  > 31) left  = 31;
    if (right > 31) right = 31;

    uint16_t val = (uint16_t)((uint16_t)left | ((uint16_t)right << 8));
    if (mute)
        val |= AC97_MUTE_BIT;

    nam_out16(channel, val);
}

void ac97_get_volume(uint16_t channel, uint8_t *left, uint8_t *right, int *mute) {
    if (!ac97_dev_present) {
        if (left)  *left  = 0;
        if (right) *right = 0;
        if (mute)  *mute  = 1;
        return;
    }

    uint16_t val = nam_in16(channel);

    if (left)  *left  = (uint8_t)(val & AC97_VOL_MASK);
    if (right) *right = (uint8_t)((val >> 8) & AC97_VOL_MASK);
    if (mute)  *mute  = (val & AC97_MUTE_BIT) ? 1 : 0;
}

int ac97_present(void) { return ac97_dev_present; }

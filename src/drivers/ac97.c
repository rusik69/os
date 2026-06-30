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
#include "sound_pcm.h"
#include "idt.h"

/* ── PCI identification ─────────────────────────────────────────── */
#define AC97_CLASS    0x04
#define AC97_SUBCLASS 0x01

/* ── Register offsets ───────────────────────────────────────────── */
/* NAM (Native Audio Mixer) — mapped to BAR0 (I/O) */
#define NAM_RESET          0x00
#define NAM_MASTER_VOL     0x02
#define NAM_PCM_VOL        0x18
#define NAM_REC_GAIN       0x1C   /* Recording gain (0-15 each channel) */
#define NAM_REC_SELECT     0x1E   /* Record source select */
#define NAM_EXTENDED_AUDIO 0x28   /* Extended audio status/control */
#define NAM_SAMPLE_RATE    0x2C   /* PCM front DAC rate */
#define NAM_PCM_IN_RATE    0x32   /* PCM ADC sample rate (if VRA enabled) */

/* Record source select values (AC97 spec 5.7.2) */
#define REC_SEL_MIC     0x0000
#define REC_SEL_CD      0x0101
#define REC_SEL_VIDEO   0x0202
#define REC_SEL_AUX     0x0303
#define REC_SEL_LINE_IN 0x0404
#define REC_SEL_STEREO  0x0505
#define REC_SEL_MONO    0x0606
#define REC_SEL_PHONE   0x0707

/* Extended audio register bits */
#define EA_VRA    (1U << 0)   /* Variable Rate Audio enable */

/* NABM (Native Audio Bus Master) — mapped to BAR1 (I/O) */
/* PCM-out (playback) registers */
#define NABM_PCM_OUT_BDBAR  0x10  /* Buffer Descriptor Base Address Register */
#define NABM_PCM_OUT_CIV    0x14  /* Current Index Value */
#define NABM_PCM_OUT_LVI    0x15  /* Last Valid Index */
#define NABM_PCM_OUT_SR     0x16  /* Status Register */
#define NABM_PCM_OUT_CR     0x1B  /* Control Register */

/* PCM-in (capture) registers */
#define NABM_PCM_IN_BDBAR   0x20  /* Buffer Descriptor Base Address Register */
#define NABM_PCM_IN_CIV     0x24  /* Current Index Value */
#define NABM_PCM_IN_LVI     0x25  /* Last Valid Index */
#define NABM_PCM_IN_SR      0x26  /* Status Register */
#define NABM_PCM_IN_CR      0x2B  /* Control Register */
#define NABM_PCM_IN_PICB    0x28  /* Position In Current Buffer */

#define NABM_GLOB_CNT       0x2C  /* Global Control */
#define NABM_GLOB_STS       0x30  /* Global Status */

#define CR_RPBM  (1U << 0)   /* Run/Pause Bus Master */
#define CR_RR    (1U << 1)   /* Reset Registers */
#define CR_LVBIE (1U << 2)   /* Last Valid Buffer Interrupt Enable */
#define CR_FEIE  (1U << 3)   /* FIFO Error Interrupt Enable */
#define CR_IOCE  (1U << 4)   /* IOC Interrupt Enable */

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

/* Capture BDL and audio buffers */
static struct ac97_bdl_entry __attribute__((aligned(4096))) cap_bdl[BDL_ENTRIES];
static int16_t __attribute__((aligned(4096))) cap_audio_buf[BDL_ENTRIES][4096];

/* ── Interrupt-driven playback state ─────────────────────────────── */
static struct pci_device         ac97_pci_dev;
static struct pci_interrupt_config ac97_int_cfg;
static int                       ac97_irq_initialized = 0;

struct ac97_playback_state {
    struct sound_pcm_stream *stream;  /** PCM stream driving the BDL ring */
    int  running;                      /** 1 if DMA engine is active */
    int  bdl_entries;                  /** Number of BDL entries in the ring */
    int  underrun;                     /** 1 if a DMA underrun occurred */
};

static struct ac97_playback_state ac97_pb;

/* Forward declaration for interrupt handler (used by ac97_init) */
static void ac97_irq_handler(struct interrupt_frame *frame);

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
static inline uint32_t nabm_in32(uint16_t reg) {
    return (uint32_t)inw(ac97_nabm_base + reg) |
           ((uint32_t)inw(ac97_nabm_base + reg + 2) << 16);
}

/* ── Init ────────────────────────────────────────────────────────── */
int ac97_init(void) {
    if (pci_find_class(AC97_CLASS, AC97_SUBCLASS, &ac97_pci_dev) < 0)
        return -1;

    ac97_nam_base  = (uint16_t)(ac97_pci_dev.bar[0] & ~0x3u);
    ac97_nabm_base = (uint16_t)(ac97_pci_dev.bar[1] & ~0x3u);
    if (!ac97_nam_base || !ac97_nabm_base) return -1;

    pci_enable_bus_master(&ac97_pci_dev);

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

    /* Enable Variable Rate Audio (VRA) for independent capture rate control */
    uint16_t ext_audio = nam_in16(NAM_EXTENDED_AUDIO);
    if (!(ext_audio & EA_VRA)) {
        nam_out16(NAM_EXTENDED_AUDIO, ext_audio | EA_VRA);
    }
    /* Set capture sample rate default */
    nam_out16(NAM_PCM_IN_RATE, 44100);

    /* Select microphone as default record source */
    nam_out16(NAM_REC_SELECT, REC_SEL_MIC);

    /* Reset PCM-out DMA engine */
    nabm_out8(NABM_PCM_OUT_CR, CR_RR);
    for (volatile uint32_t i = 0; i < 10000; i++);
    nabm_out8(NABM_PCM_OUT_CR, 0);

    ac97_dev_present = 1;
    kprintf("ac97: initialized (NAM=0x%lx, NABM=0x%lx)\n",
            (unsigned long)ac97_nam_base, (unsigned long)ac97_nabm_base);

    /* ── Set up PCI interrupts for the audio controller ──────────────── */
    if (!ac97_irq_initialized) {
        int irq_ret = pci_setup_interrupts(&ac97_pci_dev, &ac97_int_cfg,
                                            ac97_irq_handler);
        if (irq_ret == 0) {
            ac97_irq_initialized = 1;
            kprintf("[AC97] IRQ set up (type=%d, vector=%d)\n",
                    ac97_int_cfg.type, ac97_int_cfg.vector);
        } else {
            kprintf("[AC97] WARN: interrupt setup failed (%d)"
                    " — using synchronous DMA only\n", irq_ret);
        }
    }

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

/* ── Capture (Recording) ─────────────────────────────────────────── */

/**
 * ac97_capture_read — Capture audio samples from the selected input source.
 *
 * Reads audio data using a single-shot BDL-based DMA capture into the
 * provided buffer.  The capture engine records from the source previously
 * selected via ac97_set_record_source().
 *
 * @buf:   Buffer to receive 16-bit signed PCM samples (must be large enough
 *         for @bytes bytes).
 * @bytes: Number of bytes to capture (must be a multiple of 2 for 16-bit).
 * @rate:  Sample rate in Hz (8000–48000).
 *
 * Returns the number of bytes actually captured, or -1 on error.
 */
int ac97_capture_read(int16_t *buf, uint32_t bytes, uint32_t rate)
{
    if (!ac97_dev_present || !buf || bytes == 0)
        return -1;

    /* Round to even number of bytes (16-bit samples) */
    if (bytes & 1) bytes = (bytes + 1) & ~1u;
    if (bytes > sizeof(cap_audio_buf)) bytes = sizeof(cap_audio_buf);

    /* Set capture sample rate if different from current */
    if (rate != 44100) {
        uint16_t ext_audio = nam_in16(NAM_EXTENDED_AUDIO);
        if (ext_audio & EA_VRA) {
            nam_out16(NAM_PCM_IN_RATE, (uint16_t)rate);
        }
    }

    /* Reset PCM-in DMA engine */
    nabm_out8(NABM_PCM_IN_CR, CR_RR);
    for (volatile uint32_t i = 0; i < 10000; i++);
    nabm_out8(NABM_PCM_IN_CR, 0);

    /* Fill capture BDL */
    uint32_t remaining = bytes;
    uint32_t offset    = 0;
    int      n_entries = 0;

    while (remaining > 0 && n_entries < BDL_ENTRIES) {
        uint32_t chunk = remaining;
        if (chunk > sizeof(cap_audio_buf[0])) chunk = sizeof(cap_audio_buf[0]);

        cap_bdl[n_entries].addr    = (uint32_t)(uintptr_t)cap_audio_buf[n_entries];
        cap_bdl[n_entries].samples = (uint16_t)(chunk / 2); /* 16-bit samples */
        cap_bdl[n_entries].ctrl    = (n_entries == (BDL_ENTRIES - 1) || remaining - chunk == 0)
                                     ? AC97_IOC : 0;
        offset    += chunk;
        remaining -= chunk;
        n_entries++;
    }

    /* Program BDL address and LVI for capture */
    nabm_out32(NABM_PCM_IN_BDBAR, (uint32_t)(uintptr_t)cap_bdl);
    nabm_out8 (NABM_PCM_IN_LVI,   (uint8_t)(n_entries - 1));

    /* Start capture DMA */
    nabm_out8(NABM_PCM_IN_CR, CR_RPBM | CR_IOCE);

    /* Wait until capture DMA completes (busy-wait; max ~30s timeout) */
    uint32_t timeout = 0xFFFFFFF;
    uint32_t captured = 0;
    while (timeout--) {
        uint16_t sr = nabm_in16(NABM_PCM_IN_SR);
        if (sr & 0x0010) { /* IOC: interrupt on completion */
            captured = bytes;
            break;
        }
        __asm__ volatile("pause");
    }

    /* Stop capture DMA and clear status */
    nabm_out8 (NABM_PCM_IN_CR, 0);
    nabm_out16(NABM_PCM_IN_SR, 0x001E);  /* clear all status bits */

    /* Copy captured data from audio buffers into caller's buffer */
    if (captured > 0) {
        uint32_t to_copy = bytes;
        uint32_t src_off = 0;
        for (int i = 0; i < n_entries && to_copy > 0; i++) {
            uint32_t chunk = to_copy;
            if (chunk > sizeof(cap_audio_buf[0])) chunk = sizeof(cap_audio_buf[0]);
            memcpy((uint8_t *)buf + src_off, cap_audio_buf[i], chunk);
            src_off  += chunk;
            to_copy  -= chunk;
        }
    }

    /* Restore playback sample rate if it was changed */
    if (rate != 44100) {
        nam_out16(NAM_PCM_IN_RATE, 44100);
    }

    return (int)captured;
}

/**
 * ac97_set_record_source — Select the recording input source.
 *
 * @source: One of REC_SEL_MIC, REC_SEL_CD, REC_SEL_LINE_IN, etc.
 *
 * Selects which physical input (microphone, line-in, CD, etc.) is
 * routed to the capture ADC path.  The setting takes effect immediately
 * for subsequent capture_read() calls.
 */
void ac97_set_record_source(uint16_t source)
{
    if (!ac97_dev_present) return;
    nam_out16(NAM_REC_SELECT, source);
}

/**
 * ac97_set_record_gain — Set recording gain level.
 *
 * @left:   Left channel gain (0–15, where 0 = 0dB, 15 = +22.5dB).
 * @right:  Right channel gain (0–15).
 * @mute:   1 to mute recording, 0 to unmute.
 *
 * Recording gain is independent from playback volume.  The gain range
 * follows the AC97 spec: steps of 1.5dB, 0 = 0dB, 15 = +22.5dB.
 */
void ac97_set_record_gain(uint8_t left, uint8_t right, int mute)
{
    if (!ac97_dev_present) return;

    if (left  > 15) left  = 15;
    if (right > 15) right = 15;

    uint16_t val = (uint16_t)((uint16_t)left | ((uint16_t)right << 8));
    if (mute) val |= AC97_MUTE_BIT;

    nam_out16(NAM_REC_GAIN, val);
}
#include "module.h"
module_init(ac97_init);

/* ── Read AC97 register via NAM I/O ─────────────────── */
int ac97_read(int reg)
{
    if (reg < 0 || reg > 0x7E || (reg & 1))
        return -EINVAL;
    if (!ac97_dev_present)
        return -ENODEV;

    return (int)inw(ac97_nam_base + (uint16_t)reg);
}

/* ── Write AC97 register ────────────────────────────── */
int ac97_write(void *dev, uint32_t reg, uint16_t val)
{
    if (!dev) return -EINVAL;
    (void)reg; (void)val;
    return 0;
}

/* ── Reset AC97 codec (toggle RESET bit) ────────────── */
int ac97_reset(void)
{
    if (!ac97_dev_present)
        return -ENODEV;

    /* Write 1 to RESET register to initiate cold reset */
    outw(ac97_nam_base + NAM_RESET, 1);
    /* Wait for reset (simple delay loop) */
    for (volatile int _d = 0; _d < 100000; _d++) io_wait();

    /* Read back to verify reset completed */
    uint16_t status = inw(ac97_nam_base + NAM_RESET);
    if (status == 0xFFFF) {
        kprintf("[AC97] Codec reset failed\n");
        return -EIO;
    }

    kprintf("[AC97] Codec reset OK\n");
    return 0;
}

/* ── Set mixer channel level ────────────────────────── */
int ac97_mixer_set(int channel, int level)
{
    if (!ac97_dev_present)
        return -ENODEV;

    /* Map channel to register */
    uint16_t reg;
    switch (channel) {
    case 0: reg = NAM_MASTER_VOL; break;  /* master volume */
    case 1: reg = NAM_PCM_VOL;    break;  /* PCM output volume */
    case 2: reg = NAM_REC_GAIN;   break;  /* recording gain */
    default: return -EINVAL;
    }

    /* AC97 volume is 6-bit (0-63) with mute bit 15 */
    uint16_t val;
    if (level < 0) {
        val = AC97_MUTE_BIT;  /* mute */
    } else {
        if (level > 63) level = 63;
        uint16_t l = (uint16_t)level;
        uint16_t r = (uint16_t)level;
        val = l | (r << 8);
    }

    outw(ac97_nam_base + reg, val);
    return 0;
}

/* ── Interrupt handler ───────────────────────────────────────────── */

/**
 * ac97_irq_handler — Handle AC97 PCI interrupts (IOC, errors).
 *
 * Called from the IDT dispatch when the AC97 device raises an IRQ.
 * Handles playback IOC (fragment completion) and capture events.
 */
static void ac97_irq_handler(struct interrupt_frame *frame)
{
    (void)frame;

    if (!ac97_dev_present)
        return;

    /* Read and clear PCM-out (playback) status bits */
    uint16_t sr = nabm_in16(NABM_PCM_OUT_SR);
    if (sr & 0x001E)
        nabm_out16(NABM_PCM_OUT_SR, sr & 0x001E);

    /* Handle playback IOC — a BDL entry with IOC=1 completed */
    if ((sr & 0x0010) && ac97_pb.running) {
        struct sound_pcm_stream *s = ac97_pb.stream;
        if (!s) {
            ac97_pb.running = 0;
            return;
        }

        /* The DMA engine just finished one BDL entry.
         * Consume one fragment from the PCM stream. */
        sound_pcm_dma_consume(s);

        /* Determine which BDL entry just completed by reading CIV.
         * CIV points to the CURRENT entry being processed; the previous
         * entry (CIV - 1 mod count) just completed. */
        uint8_t civ = nabm_in8(NABM_PCM_OUT_CIV);
        int bdl_count = ac97_pb.bdl_entries;
        int slot = (int)(civ + bdl_count - 1) % bdl_count;

        /* Fill the just-vacated BDL slot with the next PCM fragment */
        void *frag_ptr;
        uint32_t frag_size = sound_pcm_dma_get_fragment(s, &frag_ptr);

        if (frag_ptr && frag_size > 0) {
            uint32_t to_copy = frag_size;
            if (to_copy > sizeof(audio_buf[0]))
                to_copy = sizeof(audio_buf[0]);

            memcpy(audio_buf[slot], frag_ptr, to_copy);

            /* Update BDL entry in-place (DMA wraps around) */
            bdl[slot].addr    = (uint32_t)(uintptr_t)audio_buf[slot];
            bdl[slot].samples = (uint16_t)(to_copy / 2);
            bdl[slot].ctrl    = AC97_IOC;

            ac97_pb.underrun = 0;
        } else {
            /* Underrun — fill with silence to avoid clicks/pops */
            memset(audio_buf[slot], 0, sizeof(audio_buf[0]));
            bdl[slot].samples = (uint16_t)(sizeof(audio_buf[0]) / 2);
            bdl[slot].ctrl    = AC97_IOC;
            ac97_pb.underrun  = 1;
        }
    }

    /* Clear capture status bits (even if not actively capturing) */
    uint16_t cap_sr = nabm_in16(NABM_PCM_IN_SR);
    if (cap_sr & 0x001E)
        nabm_out16(NABM_PCM_IN_SR, cap_sr & 0x001E);

    /* Clear global status */
    uint32_t gsts = nabm_in32(NABM_GLOB_STS);
    if (gsts)
        nabm_out32(NABM_GLOB_STS, gsts);

    /* EOI is handled by the IDT layer — nothing more to do */
}

/* ── Interrupt-driven playback API ───────────────────────────────── */

int ac97_playback_start(struct sound_pcm_stream *stream)
{
    if (!ac97_dev_present || !stream)
        return -EINVAL;

    if (ac97_pb.running)
        return -EBUSY;

    /* Direction must be playback */
    if (stream->dir != SOUND_PCM_PLAYBACK)
        return -EINVAL;

    /* Reset PCM-out DMA engine */
    nabm_out8(NABM_PCM_OUT_CR, CR_RR);
    for (volatile uint32_t i = 0; i < 10000; i++);
    nabm_out8(NABM_PCM_OUT_CR, 0);

    /* Clear the playback state */
    memset(&ac97_pb, 0, sizeof(ac97_pb));

    /* Fill BDL entries from the PCM stream.
     * We call get_fragment + dma_consume for each fragment:
     *   get_fragment returns pointer to data at hw_ptr
     *   dma_consume  advances hw_ptr to the next fragment
     * The data is copied to audio_buf[] so the DMA engine can
     * access it (the PCM buffer may be in a different memory region). */
    int filled = 0;
    for (int i = 0; i < BDL_ENTRIES; i++) {
        void *frag_ptr;
        uint32_t frag_size = sound_pcm_dma_get_fragment(stream, &frag_ptr);
        if (!frag_ptr || frag_size == 0)
            break;

        uint32_t to_copy = frag_size;
        if (to_copy > sizeof(audio_buf[0]))
            to_copy = sizeof(audio_buf[0]);

        memcpy(audio_buf[i], frag_ptr, to_copy);
        sound_pcm_dma_consume(stream);

        bdl[i].addr    = (uint32_t)(uintptr_t)audio_buf[i];
        bdl[i].samples = (uint16_t)(to_copy / 2);
        bdl[i].ctrl    = AC97_IOC;  /* interrupt on every fragment */

        filled++;
    }

    if (filled == 0)
        return -ENODATA;

    /* Pad remaining BDL entries with silence for a constant ring size */
    for (int i = filled; i < BDL_ENTRIES; i++) {
        memset(audio_buf[i], 0, sizeof(audio_buf[0]));
        bdl[i].addr    = (uint32_t)(uintptr_t)audio_buf[i];
        bdl[i].samples = (uint16_t)(sizeof(audio_buf[0]) / 2);
        bdl[i].ctrl    = AC97_IOC;
    }

    ac97_pb.stream      = stream;
    ac97_pb.bdl_entries = BDL_ENTRIES;
    ac97_pb.running     = 0;
    ac97_pb.underrun    = 0;

    /* Program full BDL and start DMA */
    nabm_out32(NABM_PCM_OUT_BDBAR, (uint32_t)(uintptr_t)bdl);
    nabm_out8(NABM_PCM_OUT_LVI,    (uint8_t)(BDL_ENTRIES - 1));

    /* Memory barrier: ensure BDL and BDBAR are visible before starting */
    __asm__ volatile("mfence" ::: "memory");

    nabm_out8(NABM_PCM_OUT_CR, CR_RPBM | CR_IOCE | CR_LVBIE | CR_FEIE);

    ac97_pb.running = 1;
    kprintf("[AC97] DMA playback started: %d fragments\n", filled);
    return 0;
}

void ac97_playback_stop(void)
{
    if (!ac97_dev_present)
        return;

    /* Stop DMA engine */
    nabm_out8(NABM_PCM_OUT_CR, 0);
    nabm_out16(NABM_PCM_OUT_SR, 0x001E);  /* clear all status bits */

    ac97_pb.running  = 0;
    ac97_pb.stream   = NULL;
    ac97_pb.underrun = 0;

    kprintf("[AC97] DMA playback stopped\n");
}

int ac97_playback_is_active(void)
{
    return ac97_pb.running;
}

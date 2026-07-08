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
#include "errno.h"

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

/* ── Interrupt-driven capture state ────────────────────────────────── */

struct ac97_capture_state {
    struct sound_pcm_stream *stream;  /** PCM stream receiving capture data */
    int  running;                      /** 1 if DMA capture engine is active */
    int  bdl_entries;                  /** Number of BDL entries in the ring */
    int  overrun;                      /** 1 if a capture overrun occurred */
};

static struct ac97_capture_state ac97_cap;

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

    /* ── Apply mixer defaults ──────────────────────────────────────── */
    ac97_mixer_init_defaults();

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
        bdl[n_entries].addr    = (uint32_t)VIRT_TO_PHYS(audio_buf[n_entries]);
        bdl[n_entries].samples = (uint16_t)(chunk / 2); /* 16-bit samples */
        bdl[n_entries].ctrl    = (n_entries == (BDL_ENTRIES - 1) || remaining - chunk == 0)
                                 ? AC97_IOC : 0;
        src_off   += chunk;
        remaining -= chunk;
        n_entries++;
    }

    /* Program BDL address and LVI */
    nabm_out32(NABM_PCM_OUT_BDBAR, (uint32_t)VIRT_TO_PHYS(bdl));
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

        cap_bdl[n_entries].addr    = (uint32_t)VIRT_TO_PHYS(cap_audio_buf[n_entries]);
        cap_bdl[n_entries].samples = (uint16_t)(chunk / 2); /* 16-bit samples */
        cap_bdl[n_entries].ctrl    = (n_entries == (BDL_ENTRIES - 1) || remaining - chunk == 0)
                                     ? AC97_IOC : 0;
        offset    += chunk;
        remaining -= chunk;
        n_entries++;
    }

    /* Program BDL address and LVI for capture */
    nabm_out32(NABM_PCM_IN_BDBAR, (uint32_t)VIRT_TO_PHYS(cap_bdl));
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

/* ── Write AC97 register via NAM I/O ────────────────── */
int ac97_write(uint16_t reg, uint16_t val)
{
    if (reg > 0x7E || (reg & 1))
        return -EINVAL;
    if (!ac97_dev_present)
        return -ENODEV;

    outw(ac97_nam_base + reg, val);
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
static int ac97_mixer_set(int channel, int level)
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
            bdl[slot].addr    = (uint32_t)VIRT_TO_PHYS(audio_buf[slot]);
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

    /* Handle capture IOC — a capture BDL entry just completed.
     * The DMA engine just filled one BDL entry with audio data.
     * Copy it into the PCM stream and advance the hw_ptr so the
     * application can read the captured data via sound_pcm_read(). */
    uint16_t cap_sr = nabm_in16(NABM_PCM_IN_SR);
    if ((cap_sr & 0x0010) && ac97_cap.running) {
        struct sound_pcm_stream *s = ac97_cap.stream;
        if (s) {
            /* Determine which BDL entry just completed by reading CIV.
             * CIV points to the CURRENT entry being processed; the previous
             * entry (CIV - 1 mod count) just completed. */
            uint8_t civ = nabm_in8(NABM_PCM_IN_CIV);
            int bdl_count = ac97_cap.bdl_entries;
            int slot = (int)(civ + bdl_count - 1) % bdl_count;

            /* Get a free fragment in the PCM stream to receive the data */
            void *frag_ptr;
            uint32_t frag_size = sound_pcm_dma_get_fragment(s, &frag_ptr);

            if (frag_ptr && frag_size > 0) {
                uint32_t to_copy = frag_size;
                if (to_copy > sizeof(cap_audio_buf[0]))
                    to_copy = sizeof(cap_audio_buf[0]);

                /* Copy captured data from the DMA buffer into the PCM stream */
                memcpy(frag_ptr, cap_audio_buf[slot], to_copy);

                /* Tell PCM stream that a fragment's worth of data is available */
                sound_pcm_dma_consume(s);

                ac97_cap.overrun = 0;
            } else {
                /* PCM buffer is full — discard this fragment (overrun) */
                ac97_cap.overrun = 1;
            }

            /* Reset the BDL entry so the DMA engine can re-fill it */
            memset(cap_audio_buf[slot], 0, sizeof(cap_audio_buf[0]));
            cap_bdl[slot].addr    = (uint32_t)VIRT_TO_PHYS(cap_audio_buf[slot]);
            cap_bdl[slot].samples = (uint16_t)(sizeof(cap_audio_buf[0]) / 2);
            cap_bdl[slot].ctrl    = AC97_IOC;
        } else {
            ac97_cap.running = 0;
        }
    }

    /* Clear capture status bits (even if not actively capturing) */
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

        bdl[i].addr    = (uint32_t)VIRT_TO_PHYS(audio_buf[i]);
        bdl[i].samples = (uint16_t)(to_copy / 2);
        bdl[i].ctrl    = AC97_IOC;  /* interrupt on every fragment */

        filled++;
    }

    if (filled == 0)
        return -ENODATA;

    /* Pad remaining BDL entries with silence for a constant ring size */
    for (int i = filled; i < BDL_ENTRIES; i++) {
        memset(audio_buf[i], 0, sizeof(audio_buf[0]));
        bdl[i].addr    = (uint32_t)VIRT_TO_PHYS(audio_buf[i]);
        bdl[i].samples = (uint16_t)(sizeof(audio_buf[0]) / 2);
        bdl[i].ctrl    = AC97_IOC;
    }

    ac97_pb.stream      = stream;
    ac97_pb.bdl_entries = BDL_ENTRIES;
    ac97_pb.running     = 0;
    ac97_pb.underrun    = 0;

    /* Program full BDL and start DMA */
    nabm_out32(NABM_PCM_OUT_BDBAR, (uint32_t)VIRT_TO_PHYS(bdl));
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

/* ── Interrupt-driven capture API ─────────────────────────────────── */

int ac97_capture_start(struct sound_pcm_stream *stream)
{
    if (!ac97_dev_present || !stream)
        return -EINVAL;

    if (ac97_cap.running)
        return -EBUSY;

    /* Direction must be capture */
    if (stream->dir != SOUND_PCM_CAPTURE)
        return -EINVAL;

    /* Reset PCM-in DMA engine */
    nabm_out8(NABM_PCM_IN_CR, CR_RR);
    for (volatile uint32_t i = 0; i < 10000; i++);
    nabm_out8(NABM_PCM_IN_CR, 0);

    /* Clear the capture state */
    memset(&ac97_cap, 0, sizeof(ac97_cap));

    /* Fill BDL entries with empty buffers. The DMA engine writes
     * captured audio data directly into these buffers.  When a
     * buffer is filled, an IOC interrupt fires and the interrupt
     * handler copies the data into the PCM stream for the
     * application to read via sound_pcm_read(). */
    for (int i = 0; i < BDL_ENTRIES; i++) {
        memset(cap_audio_buf[i], 0, sizeof(cap_audio_buf[0]));
        cap_bdl[i].addr    = (uint32_t)VIRT_TO_PHYS(cap_audio_buf[i]);
        cap_bdl[i].samples = (uint16_t)(sizeof(cap_audio_buf[0]) / 2);
        cap_bdl[i].ctrl    = AC97_IOC;
    }

    ac97_cap.stream      = stream;
    ac97_cap.bdl_entries = BDL_ENTRIES;
    ac97_cap.running     = 0;
    ac97_cap.overrun     = 0;

    /* Program full BDL and start capture DMA */
    nabm_out32(NABM_PCM_IN_BDBAR, (uint32_t)VIRT_TO_PHYS(cap_bdl));
    nabm_out8(NABM_PCM_IN_LVI,    (uint8_t)(BDL_ENTRIES - 1));

    /* Memory barrier: ensure BDL and BDBAR are visible before starting */
    __asm__ volatile("mfence" ::: "memory");

    nabm_out8(NABM_PCM_IN_CR, CR_RPBM | CR_IOCE | CR_LVBIE | CR_FEIE);

    ac97_cap.running = 1;
    kprintf("[AC97] DMA capture started: %d BDL entries\n", BDL_ENTRIES);
    return 0;
}

void ac97_capture_stop(void)
{
    if (!ac97_dev_present)
        return;

    /* Stop capture DMA engine */
    nabm_out8(NABM_PCM_IN_CR, 0);
    nabm_out16(NABM_PCM_IN_SR, 0x001E);  /* clear all status bits */

    ac97_cap.running  = 0;
    ac97_cap.stream   = NULL;
    ac97_cap.overrun  = 0;

    kprintf("[AC97] DMA capture stopped\n");
}

int ac97_capture_is_active(void)
{
    return ac97_cap.running;
}

/* ── AC97 mixer implementation ────────────────────────────────────── */

/**
 * Channel-to-register mapping table.
 * Maps each ac97_mixer_channel enum value to its NAM register offset.
 * 0 means "no direct register" — the channel is virtual or handled
 * via a different mechanism.
 */
static const uint16_t ac97_channel_regs[AC97_MIXER_CHANNEL_COUNT] = {
    [AC97_MIXER_MASTER]     = AC97_REG_MASTER,
    [AC97_MIXER_AUX_OUT]    = AC97_REG_AUX_OUT,
    [AC97_MIXER_MASTER_MONO]= AC97_REG_MASTER_MONO,
    [AC97_MIXER_PC_BEEP]    = AC97_REG_PC_BEEP,
    [AC97_MIXER_PHONE]      = AC97_REG_PHONE,
    [AC97_MIXER_MIC]        = AC97_REG_MIC,
    [AC97_MIXER_LINE_IN]    = AC97_REG_LINE_IN,
    [AC97_MIXER_CD]         = AC97_REG_CD,
    [AC97_MIXER_VIDEO]      = AC97_REG_VIDEO,
    [AC97_MIXER_AUX]        = AC97_REG_AUX,
    [AC97_MIXER_PCM]        = AC97_REG_PCM_OUT,
    [AC97_MIXER_RECORD_GAIN]= AC97_REG_RECORD_GAIN,
};

/**
 * Human-readable channel names.
 */
static const char * const ac97_channel_names[AC97_MIXER_CHANNEL_COUNT] = {
    [AC97_MIXER_MASTER]      = "Master",
    [AC97_MIXER_AUX_OUT]     = "Aux Out",
    [AC97_MIXER_MASTER_MONO] = "Master Mono",
    [AC97_MIXER_PC_BEEP]     = "PC Beep",
    [AC97_MIXER_PHONE]       = "Phone",
    [AC97_MIXER_MIC]         = "Mic",
    [AC97_MIXER_LINE_IN]     = "Line In",
    [AC97_MIXER_CD]          = "CD",
    [AC97_MIXER_VIDEO]       = "Video",
    [AC97_MIXER_AUX]         = "Aux",
    [AC97_MIXER_PCM]         = "PCM",
    [AC97_MIXER_RECORD_GAIN] = "Record Gain",
};

/**
 * ac97_mixer_get_channel_register — Return the NAM register offset for a channel.
 */
uint16_t ac97_mixer_get_channel_register(enum ac97_mixer_channel ch)
{
    if (ch < 0 || ch >= AC97_MIXER_CHANNEL_COUNT)
        return 0;
    return ac97_channel_regs[(int)ch];
}

/**
 * ac97_mixer_get_channel_name — Return a human-readable name for a channel.
 */
const char *ac97_mixer_get_channel_name(enum ac97_mixer_channel ch)
{
    if (ch < 0 || ch >= AC97_MIXER_CHANNEL_COUNT)
        return "Unknown";
    return ac97_channel_names[(int)ch];
}

/* ── Gain conversion helpers ─────────────────────────────────────── */

/**
 * pct_to_ac97_gain — Convert 0–100% to AC97 5-bit volume (0=max, 31=min).
 *
 * AC97 volume registers use inverted scaling: 0 = 0dB (max), 31 = -46.5dB (min).
 * Each step is 1.5dB.  We map linearly:
 *   0% -> 31 (silent)
 *   100% -> 0 (0dB max)
 */
static inline uint8_t pct_to_ac97_5bit(uint8_t pct)
{
    if (pct >= 100) return 0;       /* max volume (0dB) */
    if (pct == 0)   return 31;      /* min volume (-46.5dB) */
    return (uint8_t)(31U - (pct * 31U / 100U));
}

/**
 * ac97_5bit_to_pct — Convert AC97 5-bit volume to 0–100%.
 */
static inline uint8_t ac97_5bit_to_pct(uint8_t raw)
{
    if (raw == 0)   return 100;     /* max */
    if (raw >= 31)  return 0;       /* min */
    return (uint8_t)((31U - raw) * 100U / 31U);
}

/**
 * pct_to_ac97_gain_4bit — Convert 0–100% to AC97 4-bit recording gain (0–15).
 *
 * AC97 recording gain: 0 = 0dB, 15 = +22.5dB (1.5dB steps).
 * We map: 0% -> 0 (0dB), 100% -> 15 (+22.5dB).
 */
static inline uint8_t pct_to_ac97_4bit(uint8_t pct)
{
    if (pct >= 100) return 15;
    return (uint8_t)(pct * 15U / 100U);
}

/**
 * ac97_4bit_to_pct — Convert AC97 4-bit gain to 0–100%.
 */
static inline uint8_t ac97_4bit_to_pct(uint8_t raw)
{
    if (raw >= 15) return 100;
    return (uint8_t)(raw * 100U / 15U);
}

/* ── Mixer API implementations ────────────────────────────────────── */

int ac97_mixer_set_volume(enum ac97_mixer_channel ch, uint8_t left, uint8_t right)
{
    if (ch < 0 || ch >= AC97_MIXER_CHANNEL_COUNT)
        return -EINVAL;
    if (!ac97_dev_present)
        return -ENODEV;

    uint16_t reg = ac97_channel_regs[(int)ch];
    if (reg == 0)
        return -EINVAL;  /* No direct register mapping */

    if (left  > 100) left  = 100;
    if (right > 100) right = 100;

    /* Convert to AC97 5-bit raw values (0=max, 31=min) */
    uint8_t raw_left  = pct_to_ac97_5bit(left);
    uint8_t raw_right = pct_to_ac97_5bit(right);

    /* Build register value, preserving mute state */
    uint16_t cur = inw(ac97_nam_base + reg);
    uint16_t mute_bit = cur & AC97_VOL_MUTE_BIT;

    uint16_t val = (uint16_t)raw_left | ((uint16_t)raw_right << 8) | mute_bit;
    outw(ac97_nam_base + reg, val);

    return 0;
}

int ac97_mixer_get_volume(enum ac97_mixer_channel ch, uint8_t *left, uint8_t *right)
{
    if (ch < 0 || ch >= AC97_MIXER_CHANNEL_COUNT)
        return -EINVAL;
    if (!ac97_dev_present)
        return -ENODEV;

    uint16_t reg = ac97_channel_regs[(int)ch];
    if (reg == 0)
        return -EINVAL;

    uint16_t val = inw(ac97_nam_base + reg);

    uint8_t raw_left  = (uint8_t)(val & AC97_VOL_LEFT_MASK);
    uint8_t raw_right = (uint8_t)((val & AC97_VOL_RIGHT_MASK) >> 8);

    if (left)  *left  = ac97_5bit_to_pct(raw_left);
    if (right) *right = ac97_5bit_to_pct(raw_right);

    return 0;
}

int ac97_mixer_set_mute(enum ac97_mixer_channel ch, int mute)
{
    if (ch < 0 || ch >= AC97_MIXER_CHANNEL_COUNT)
        return -EINVAL;
    if (!ac97_dev_present)
        return -ENODEV;

    uint16_t reg = ac97_channel_regs[(int)ch];
    if (reg == 0)
        return -EINVAL;

    uint16_t val = inw(ac97_nam_base + reg);
    if (mute)
        val |= AC97_VOL_MUTE_BIT;
    else
        val &= ~AC97_VOL_MUTE_BIT;

    outw(ac97_nam_base + reg, val);
    return 0;
}

int ac97_mixer_get_mute(enum ac97_mixer_channel ch, int *mute)
{
    if (ch < 0 || ch >= AC97_MIXER_CHANNEL_COUNT)
        return -EINVAL;
    if (!ac97_dev_present)
        return -ENODEV;

    if (!mute)
        return -EINVAL;

    uint16_t reg = ac97_channel_regs[(int)ch];
    if (reg == 0)
        return -EINVAL;

    uint16_t val = inw(ac97_nam_base + reg);
    *mute = (val & AC97_VOL_MUTE_BIT) ? 1 : 0;

    return 0;
}

int ac97_mixer_set_mic_boost(int enable)
{
    if (!ac97_dev_present)
        return -ENODEV;

    uint16_t val = inw(ac97_nam_base + AC97_REG_MIC);
    if (enable)
        val |= AC97_MIC_20DB_BIT;
    else
        val &= ~AC97_MIC_20DB_BIT;

    outw(ac97_nam_base + AC97_REG_MIC, val);
    return 0;
}

int ac97_mixer_get_mic_boost(int *enabled)
{
    if (!ac97_dev_present)
        return -ENODEV;
    if (!enabled)
        return -EINVAL;

    uint16_t val = inw(ac97_nam_base + AC97_REG_MIC);
    *enabled = (val & AC97_MIC_20DB_BIT) ? 1 : 0;

    return 0;
}

int ac97_mixer_set_tone(uint8_t bass, uint8_t treble)
{
    if (!ac97_dev_present)
        return -ENODEV;

    /* Clamp to 4-bit range */
    if (bass   > 15) bass   = 15;
    if (treble > 15) treble = 15;

    /* Tone control register: bits [7:0] = left (bass), [15:8] = right (treble)
     * Each nibble: 0 = flat, 1-15 = cut/boost (vendor-specific scaling). */
    uint16_t val = (uint16_t)bass | ((uint16_t)treble << 8);

    /* Write bass to AC97_REG_MASTER_TONE_L and treble to AC97_REG_MASTER_TONE_R.
     * Note: both registers share offset 0x08, but the spec defines
     * MASTER_TONE_L at 0x08 and MASTER_TONE_R at 0x0A (PC_BEEP overlap).
     * In practice, many codecs use a single 16-bit register at 0x08. */
    outw(ac97_nam_base + AC97_REG_MASTER_TONE_L, val);

    return 0;
}

int ac97_mixer_set_record_gain(enum ac97_mixer_channel ch, uint8_t left, uint8_t right, int mute)
{
    if (ch < 0 || ch >= AC97_MIXER_CHANNEL_COUNT)
        return -EINVAL;
    if (!ac97_dev_present)
        return -ENODEV;

    /* Only RECORD_GAIN channel controls recording gain level.
     * Other channels use the standard volume register format. */
    if (ch == AC97_MIXER_RECORD_GAIN) {
        if (left  > 100) left  = 100;
        if (right > 100) right = 100;

        /* Recording gain is 4-bit (0–15), mapped from 0–100% */
        uint8_t raw_left  = pct_to_ac97_4bit(left);
        uint8_t raw_right = pct_to_ac97_4bit(right);

        uint16_t val = (uint16_t)raw_left | ((uint16_t)raw_right << 8);
        if (mute) val |= AC97_VOL_MUTE_BIT;

        outw(ac97_nam_base + AC97_REG_RECORD_GAIN, val);
        return 0;
    }

    /* For other channels, use standard volume/mute control */
    return ac97_mixer_set_volume(ch, left, right);
}

void ac97_mixer_init_defaults(void)
{
    if (!ac97_dev_present)
        return;

    /* Set all channels to default volume (75%), unmuted */
    for (int i = 0; i < AC97_MIXER_CHANNEL_COUNT; i++) {
        uint16_t reg = ac97_channel_regs[i];
        if (reg == 0) continue;

        /* Initialise volume to default (75% == ~8 raw AC97) */
        uint8_t raw_default = pct_to_ac97_5bit(AC97_VOLUME_DEFAULT);
        uint16_t val = (uint16_t)raw_default | ((uint16_t)raw_default << 8);

        /* All stereo/mono volume registers are unmuted by default */
        outw(ac97_nam_base + reg, val);
    }

    /* Special handling for PC_BEEP: also set beep enable bit */
    uint16_t beep_val = inw(ac97_nam_base + AC97_REG_PC_BEEP);
    beep_val |= AC97_BEEP_ENABLE_BIT;  /* enable beep passthrough */
    outw(ac97_nam_base + AC97_REG_PC_BEEP, beep_val);

    /* Disable mic 20dB boost by default */
    uint16_t mic_val = inw(ac97_nam_base + AC97_REG_MIC);
    mic_val &= ~AC97_MIC_20DB_BIT;
    outw(ac97_nam_base + AC97_REG_MIC, mic_val);

    /* Select microphone as default recording source */
    outw(ac97_nam_base + AC97_REG_RECORD_SOURCE, REC_SEL_MIC);

    /* Set recording gain to default (75% of 0–15 = ~11, ~16.5dB) */
    uint8_t raw_gain = pct_to_ac97_4bit(AC97_VOLUME_DEFAULT);
    outw(ac97_nam_base + AC97_REG_RECORD_GAIN, (uint16_t)raw_gain | ((uint16_t)raw_gain << 8));

    kprintf("[AC97] Mixer defaults applied (%d channels)\\n", AC97_MIXER_CHANNEL_COUNT);
}

/* ── ac97_init now also calls mixer defaults ──────────────────────── */

/* ═══════════════════════════════════════════════════════════════════════
 *  Power Management — Cold Reset, Warm Resume, Suspend/Resume
 * ═══════════════════════════════════════════════════════════════════════ */

/**
 * Current AC97 power management state.
 * Starts at D0 (fully on) after ac97_init completes.
 */
static enum ac97_power_state ac97_pwr_state = AC97_POWER_D0;

/**
 * Saved mixer state for restore on resume.
 * The AC97 spec guarantees that mixer registers survive a warm reset,
 * but not a cold reset.  We save the primary channel volumes before
 * D3_COLD suspend so they can be restored after cold reset.
 */
struct ac97_saved_mixer_state {
    uint16_t master;
    uint16_t pcm;
    uint16_t mic;
    uint16_t line_in;
    uint16_t cd;
    uint16_t rec_gain;
    uint16_t rec_source;
    uint16_t ext_audio;
};

static struct ac97_saved_mixer_state ac97_saved_regs;

/**
 * ac97_cold_reset — Full AC-link cold reset.
 *
 * Toggles the cold reset bit in the NABM Global Control register,
 * which forces a complete AC-link reset.  All codec registers return
 * to their hardware-default values.  After the reset completes, the
 * codec vendor ID is read to confirm the codec is responsive.
 */
int ac97_cold_reset(void)
{
    if (!ac97_dev_present)
        return -ENODEV;

    /* Stop any active DMA before resetting */
    nabm_out8(NABM_PCM_OUT_CR, 0);
    nabm_out8(NABM_PCM_IN_CR, 0);
    nabm_out16(NABM_PCM_OUT_SR, 0x001E);
    nabm_out16(NABM_PCM_IN_SR, 0x001E);

    /* Assert cold reset (write 1 to bit 1 of NABM Glob_Cnt) */
    nabm_out32(NABM_GLOB_CNT, AC97_GC_COLD_RESET);

    /* Hold reset for ~1us (busy-loop) */
    for (volatile uint32_t i = 0; i < 100; i++)
        io_wait();

    /* Release reset */
    nabm_out32(NABM_GLOB_CNT, 0);
    ac97_pwr_state = AC97_POWER_D3_COLD;

    /* Wait for AC-link to stabilise (AC97 spec: 128 SYNC cycles = ~32us) */
    for (volatile uint32_t i = 0; i < 50000; i++)
        io_wait();

    /* Verify codec is responsive by reading vendor ID registers */
    uint16_t vid1 = inw(ac97_nam_base + AC97_REG_VENDOR_ID1);
    uint16_t vid2 = inw(ac97_nam_base + AC97_REG_VENDOR_ID2);

    if (vid1 == 0xFFFF && vid2 == 0xFFFF) {
        kprintf("[AC97] Cold reset failed — codec not responding\n");
        return -EIO;
    }

    ac97_pwr_state = AC97_POWER_D0;
    kprintf("[AC97] Cold reset OK (vendor=0x%04x:0x%04x)\n",
            (unsigned)vid1, (unsigned)vid2);
    return 0;
}

/**
 * ac97_warm_reset — Warm reset of the AC-link.
 *
 * Toggles the warm reset bit in NABM Global Control (bit 2) to
 * reinitialise the AC-link protocol without resetting codec registers.
 * All mixer and volume settings are preserved across a warm reset.
 *
 * This is the preferred resume path when the codec was suspended
 * to D3 (AC-Link powerdown via PR3).  It is substantially faster
 * than a cold reset.
 */
int ac97_warm_reset(void)
{
    if (!ac97_dev_present)
        return -ENODEV;

    /* Assert warm reset (write 1 to bit 2 of NABM Glob_Cnt) */
    nabm_out32(NABM_GLOB_CNT, AC97_GC_WARM_RESET);

    /* Hold warm reset for at least 1us */
    for (volatile uint32_t i = 0; i < 100; i++)
        io_wait();

    /* Release warm reset */
    nabm_out32(NABM_GLOB_CNT, 0);

    /* Wait for AC-link to synchronise (~128 SYNC frames @ 48kHz = ~2.67ms) */
    for (volatile uint32_t i = 0; i < 100000; i++)
        io_wait();

    /* Verify codec is now responding */
    uint16_t vid1 = inw(ac97_nam_base + AC97_REG_VENDOR_ID1);
    uint16_t vid2 = inw(ac97_nam_base + AC97_REG_VENDOR_ID2);

    if (vid1 == 0xFFFF && vid2 == 0xFFFF) {
        kprintf("[AC97] Warm reset failed — codec not responding\n");
        return -EIO;
    }

    ac97_pwr_state = AC97_POWER_D0;
    kprintf("[AC97] Warm reset OK\n");
    return 0;
}

/**
 * ac97_suspend — Suspend the AC97 device.
 *
 * Powers down audio functions according to the requested ACPI-style
 * power state:
 *
 *   D1 (suspend ADC):
 *       Powers down the ADC (PR0).  DAC and mixer remain active.
 *       Allows playback to continue but recording is disabled.
 *
 *   D2 (deep sleep):
 *       Powers down ADC (PR0), DAC (PR1), and analog mixer (PR2).
 *       All audio functions are off.  Internal clocks may also be
 *       disabled (PR4).  Wakeup requires clearing PR bits.
 *
 *   D3 (AC-Link off):
 *       Powers down the entire AC-link (PR3) plus all functions.
 *       Only the wakeup logic (PR5) remains active.
 *       Resume requires ac97_warm_reset().
 *
 *   D3_COLD (full off):
 *       Like D3, but the caller intends to follow with ac97_cold_reset()
 *       to resume.  Used for system-wide suspend-to-RAM/disk.
 */
int ac97_suspend(enum ac97_power_state state)
{
    if (!ac97_dev_present)
        return -ENODEV;

    if (state < AC97_POWER_D1 || state > AC97_POWER_D3_COLD)
        return -EINVAL;

    /* Stop any active DMA */

    /* Save current mixer state for potential restore */
    ac97_saved_regs.master     = inw(ac97_nam_base + AC97_REG_MASTER);
    ac97_saved_regs.pcm        = inw(ac97_nam_base + AC97_REG_PCM_OUT);
    ac97_saved_regs.mic        = inw(ac97_nam_base + AC97_REG_MIC);
    ac97_saved_regs.line_in    = inw(ac97_nam_base + AC97_REG_LINE_IN);
    ac97_saved_regs.cd         = inw(ac97_nam_base + AC97_REG_CD);
    ac97_saved_regs.rec_gain   = inw(ac97_nam_base + AC97_REG_RECORD_GAIN);
    ac97_saved_regs.rec_source = inw(ac97_nam_base + AC97_REG_RECORD_SOURCE);
    ac97_saved_regs.ext_audio  = inw(ac97_nam_base + AC97_REG_EXT_AUDIO_CTRL);

    switch (state) {
    case AC97_POWER_D1:
        /* Power down ADC only (PR0) */
        outw(ac97_nam_base + AC97_REG_POWERDOWN, AC97_PD_PR0);
        kprintf("[AC97] Suspend to D1: ADC powered down\n");
        break;

    case AC97_POWER_D2:
        /* Power down ADC, DAC, analog mixer, VREF */
        outw(ac97_nam_base + AC97_REG_POWERDOWN,
             AC97_PD_PR0 | AC97_PD_PR1 | AC97_PD_PR2 | AC97_PD_PR4 | AC97_PD_PR6);
        kprintf("[AC97] Suspend to D2: deep sleep\n");
        break;

    case AC97_POWER_D3:
        /* Stop DMA engines first */
        nabm_out8(NABM_PCM_OUT_CR, 0);
        nabm_out8(NABM_PCM_IN_CR, 0);

        /* Power down everything including AC-Link */
        outw(ac97_nam_base + AC97_REG_POWERDOWN, AC97_PD_ALL);

        /* Also power down external amplifier */
        uint16_t pd = inw(ac97_nam_base + AC97_REG_POWERDOWN);
        pd |= AC97_PD_EAPD;
        outw(ac97_nam_base + AC97_REG_POWERDOWN, pd);

        kprintf("[AC97] Suspend to D3: AC-Link off\n");
        break;

    case AC97_POWER_D3_COLD:
        /* Stop DMA engines */
        nabm_out8(NABM_PCM_OUT_CR, 0);
        nabm_out8(NABM_PCM_IN_CR, 0);

        /* Power down AC-Link */
        outw(ac97_nam_base + AC97_REG_POWERDOWN, AC97_PD_ALL);

        /* Perform cold reset to fully power off */
        nabm_out32(NABM_GLOB_CNT, AC97_GC_COLD_RESET);
        for (volatile uint32_t i = 0; i < 100; i++)
            io_wait();
        nabm_out32(NABM_GLOB_CNT, 0);

        kprintf("[AC97] Suspend to D3_COLD: fully off\n");
        break;

    default:
        return -EINVAL;
    }

    ac97_pwr_state = state;
    return 0;
}

/**
 * ac97_resume — Resume the AC97 device from a suspended state.
 *
 * Selects the appropriate resume method based on current power state:
 *   D3_COLD -> ac97_cold_reset() + restore saved mixer settings
 *   D3      -> ac97_warm_reset()  (mixer state preserved by spec)
 *   D1/D2   -> Clear PR bits (immediate wakeup)
 *
 * After resume, mixer defaults are re-applied to ensure consistent
 * audio state regardless of how much state survived the transition.
 */
int ac97_resume(void)
{
    int ret;

    if (!ac97_dev_present)
        return -ENODEV;

    if (ac97_pwr_state == AC97_POWER_D0) {
        kprintf("[AC97] Resume: already in D0, nothing to do\n");
        return 0;
    }

    switch (ac97_pwr_state) {
    case AC97_POWER_D3_COLD:
        /* Cold reset brings the codec back from full power-off */
        ret = ac97_cold_reset();
        if (ret < 0)
            return ret;

        /* Restore mixer state (cold reset clears all registers) */
        outw(ac97_nam_base + AC97_REG_MASTER,      ac97_saved_regs.master);
        outw(ac97_nam_base + AC97_REG_PCM_OUT,     ac97_saved_regs.pcm);
        outw(ac97_nam_base + AC97_REG_MIC,         ac97_saved_regs.mic);
        outw(ac97_nam_base + AC97_REG_LINE_IN,     ac97_saved_regs.line_in);
        outw(ac97_nam_base + AC97_REG_CD,          ac97_saved_regs.cd);
        outw(ac97_nam_base + AC97_REG_RECORD_GAIN, ac97_saved_regs.rec_gain);
        outw(ac97_nam_base + AC97_REG_RECORD_SOURCE, ac97_saved_regs.rec_source);

        /* Re-enable VRA if it was active */
        if (ac97_saved_regs.ext_audio & EA_VRA) {
            uint16_t ext = inw(ac97_nam_base + AC97_REG_EXT_AUDIO_CTRL);
            outw(ac97_nam_base + AC97_REG_EXT_AUDIO_CTRL, ext | EA_VRA);
        }

        /* Re-apply mixer defaults as a safety net */
        ac97_mixer_init_defaults();
        kprintf("[AC97] Resume from D3_COLD: mixer state restored\n");
        break;

    case AC97_POWER_D3:
        /* Warm reset preserves mixer state per AC97 spec */
        ret = ac97_warm_reset();
        if (ret < 0)
            return ret;

        /* Re-apply mixer defaults in case some settings were lost */
        ac97_mixer_init_defaults();
        kprintf("[AC97] Resume from D3: warm reset OK\n");
        break;

    case AC97_POWER_D2:
    case AC97_POWER_D1:
        /* Clear all powerdown bits to resume audio functions */
        outw(ac97_nam_base + AC97_REG_POWERDOWN, 0);

        /* Wait for PLL and clocks to stabilise */
        for (volatile uint32_t i = 0; i < 50000; i++)
            io_wait();

        ac97_mixer_init_defaults();
        kprintf("[AC97] Resume from %s: powered up\n",
                ac97_pwr_state == AC97_POWER_D2 ? "D2" : "D1");
        break;

    default:
        return -EINVAL;
    }

    ac97_pwr_state = AC97_POWER_D0;
    return 0;
}

/**
 * ac97_get_power_state — Return the current power state.
 */
enum ac97_power_state ac97_get_power_state(void)
{
    return ac97_pwr_state;
}

/**
 * ac97_set_amplifier_power — Control external amplifier power via EAPD bit.
 *
 * The EAPD bit (bit 15 of AC97_REG_POWERDOWN) controls an external
 * amplifier power-down signal.  Setting EAPD=1 powers down the external
 * amp; clearing it powers up.  Not all codecs implement this.
 */
int ac97_set_amplifier_power(int on)
{
    if (!ac97_dev_present)
        return -ENODEV;

    uint16_t pd = inw(ac97_nam_base + AC97_REG_POWERDOWN);

    if (on) {
        /* Power up: clear EAPD bit (EAPD=0 = amp on) */
        pd &= ~AC97_PD_EAPD;
    } else {
        /* Power down: set EAPD bit (EAPD=1 = amp off) */
        pd |= AC97_PD_EAPD;
    }

    outw(ac97_nam_base + AC97_REG_POWERDOWN, pd);

    kprintf("[AC97] External amplifier %s\n", on ? "ON" : "OFF");
    return 0;
}

MODULE_LICENSE("GPL");
MODULE_VERSION("1.0.0");
MODULE_DESCRIPTION("AC97 audio controller driver");
MODULE_AUTHOR("OS Kernel Team");

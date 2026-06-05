/*
 * sound_core.c — Sound subsystem core: mixer interface
 *
 * Implements a unified audio mixer abstraction with:
 *   - Per-channel volume (0..100), mute, and recording-select state
 *   - /sys/class/sound/ hierarchy for userspace mixer control
 *   - Hardware codec sync (AC97 NAM registers if present)
 *   - Software-only fallback for PC speaker / beep
 *
 * Each mixer channel gets writable sysfs files:
 *   /sys/class/sound/controlC0/<channel>/volume
 *   /sys/class/sound/controlC0/<channel>/mute
 *
 * Writing "75" to volume sets both L+R to 75%.
 * Writing "L50,R80" sets L=50, R=80.
 * Writing "L50 R80" sets L=50, R=80 (space separator).
 * Writing "1" to mute mutes, "0" unmutes.
 *
 * Item 227 — Sound core mixer interface (Plan 4, 200-more-production-improvements)
 */

#include "sound_core.h"
#include "ac97.h"
#include "sysfs.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"
#include "stdlib.h"

/* ── Global mixer state ───────────────────────────────────────────── */

/** Array of per-channel mixer states, one per SOUND_MIXER_* channel */
struct sound_mixer_channel_state g_sound_mixer[SOUND_MIXER_COUNT];

/** Protects the mixer state from concurrent access */
static spinlock_t g_mixer_lock;

/* Channel name strings used for sysfs directory names */
static const char * const ch_names[SOUND_MIXER_COUNT] = {
    "master",
    "pcm",
    "mic",
    "linein",
    "cd",
    "speaker"
};

/* ── Hardware sync helpers ────────────────────────────────────────── */

/**
 * sync_to_hardware — Push software mixer state to hardware codec.
 * @ch:  Mixer channel to synchronise.
 *
 * For the AC97 codec, maps the 0..100 software volume range to the
 * AC97 raw 0..31 register range.  AC97 uses 0 = max volume (0dB) and
 * 31 = min (-46.5dB silence).  We invert: software 100 -> AC97 0 (max),
 * software 0 -> AC97 31 (min).
 */
static void sync_to_hardware(enum sound_mixer_channel ch)
{
    if (!ac97_present())
        return;

    /* Map software channel to AC97 NAM register */
    uint16_t ac97_reg;
    switch (ch) {
        case SOUND_MIXER_MASTER:  ac97_reg = AC97_MIXER_MASTER; break;
        case SOUND_MIXER_PCM:     ac97_reg = AC97_MIXER_PCM;    break;
        case SOUND_MIXER_MIC:     ac97_reg = AC97_MIXER_MIC;    break;
        case SOUND_MIXER_LINE_IN: ac97_reg = AC97_MIXER_LINE_IN; break;
        case SOUND_MIXER_CD:      ac97_reg = AC97_MIXER_CD;     break;
        default:
            return; /* No hardware mapping for SPEAKER or unknown */
    }

    struct sound_mixer_channel_state *st = &g_sound_mixer[ch];

    /* Map 0..100 -> 0..31 (inverted: 0=max, 31=min) */
    uint8_t raw_left  = (uint8_t)(31 - (st->left  * 31U / 100U));
    uint8_t raw_right = (uint8_t)(31 - (st->right * 31U / 100U));

    ac97_set_volume(ac97_reg, raw_left, raw_right, st->mute);
}

/**
 * sync_from_hardware — Read hardware codec state into software mixer.
 * @ch:  Mixer channel to synchronise.
 *
 * Called on init to populate software state from AC97 reset values.
 */
static void sync_from_hardware(enum sound_mixer_channel ch)
{
    if (!ac97_present())
        return;

    uint16_t ac97_reg;
    switch (ch) {
        case SOUND_MIXER_MASTER:  ac97_reg = AC97_MIXER_MASTER; break;
        case SOUND_MIXER_PCM:     ac97_reg = AC97_MIXER_PCM;    break;
        case SOUND_MIXER_MIC:     ac97_reg = AC97_MIXER_MIC;    break;
        case SOUND_MIXER_LINE_IN: ac97_reg = AC97_MIXER_LINE_IN; break;
        case SOUND_MIXER_CD:      ac97_reg = AC97_MIXER_CD;     break;
        default:
            return;
    }

    uint8_t raw_left, raw_right;
    int mute;
    ac97_get_volume(ac97_reg, &raw_left, &raw_right, &mute);

    struct sound_mixer_channel_state *st = &g_sound_mixer[ch];

    /* Map 0..31 -> 0..100 (inverted: 0=max -> 100, 31=min -> 0) */
    st->left  = (uint8_t)((31U - raw_left)  * 100U / 31U);
    st->right = (uint8_t)((31U - raw_right) * 100U / 31U);
    st->mute  = (uint8_t)mute;
}

/* ── String parsing helper ────────────────────────────────────────── */

/**
 * parse_simple_integer — Parse a decimal integer from a string.
 * @str:  NUL-terminated string.
 * @val:  Receives the parsed value.
 *
 * Returns pointer to first non-digit character, or NULL on error.
 */
static const char *parse_simple_integer(const char *str, int *val)
{
    if (!str || !*str)
        return NULL;
    int sign = 1;
    const char *p = str;
    if (*p == '-') { sign = -1; p++; }
    else if (*p == '+') { p++; }
    if (*p < '0' || *p > '9')
        return NULL;
    int v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10 + (*p - '0');
        p++;
    }
    *val = v * sign;
    return p;
}

/**
 * skip_spaces — Advance past space characters.
 */
static const char *skip_spaces(const char *p)
{
    while (p && *p && (*p == ' ' || *p == '\t'))
        p++;
    return p;
}

/* ── Sysfs callbacks ──────────────────────────────────────────────── */

/**
 * Read callback for /sys/class/sound/controlC0/<channel>/volume.
 * Returns "L<left> R<right>\n" text.
 */
static int volume_read_cb(char *buf, uint32_t max_size, void *priv)
{
    enum sound_mixer_channel ch = (enum sound_mixer_channel)(uintptr_t)priv;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_mixer_lock, &irq_flags);
    struct sound_mixer_channel_state *st = &g_sound_mixer[ch];
    int n = snprintf(buf, (size_t)max_size, "L%u R%u\n",
                     (unsigned)st->left, (unsigned)st->right);
    spinlock_irqsave_release(&g_mixer_lock, irq_flags);

    return (n < 0) ? 0 : n;
}

/**
 * Write callback for /sys/class/sound/controlC0/<channel>/volume.
 * Accepted formats:
 *   "75"        -> set both L+R to 75
 *   "L50,R80"   -> set L=50, R=80
 *   "L50 R80"   -> same (space separator)
 */
static int volume_write_cb(const char *buf, uint32_t size, void *priv)
{
    enum sound_mixer_channel ch = (enum sound_mixer_channel)(uintptr_t)priv;

    /* Copy to a temporary NUL-terminated buffer */
    char tmp[32];
    uint32_t n = size;
    if (n > sizeof(tmp) - 1) n = sizeof(tmp) - 1;
    memcpy(tmp, buf, n);
    tmp[n] = '\0';

    /* Trim trailing whitespace/newline */
    while (n > 0 && (tmp[n - 1] == '\n' || tmp[n - 1] == '\r' || tmp[n - 1] == ' '))
        tmp[--n] = '\0';

    uint8_t left, right;
    int found = 0;

    if (tmp[0] == 'L' || tmp[0] == 'l') {
        /* Format: L<left>[ ,]R<right> */
        int lval = 0, rval = 0;
        const char *p = parse_simple_integer(tmp + 1, &lval);
        if (p) {
            p = skip_spaces(p);
            if (*p == ',') p = skip_spaces(p + 1);
            if (*p == 'R' || *p == 'r')
                p = parse_simple_integer(p + 1, &rval);
        }
        if (p && rval >= 0) {
            if (lval < 0) lval = 0;
            if (lval > 100) lval = 100;
            if (rval < 0) rval = 0;
            if (rval > 100) rval = 100;
            left  = (uint8_t)lval;
            right = (uint8_t)rval;
            found = 1;
        }
    } else {
        /* Simple number: set both channels */
        int v = atoi(tmp);
        if (v > 0 || *tmp == '0' || (v == 0 && *tmp >= '0' && *tmp <= '9')) {
            if (v < 0) v = 0;
            if (v > 100) v = 100;
            left = right = (uint8_t)v;
            found = 1;
        }
    }

    if (!found)
        return (int)size; /* Ignore unparseable */

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_mixer_lock, &irq_flags);
    g_sound_mixer[ch].left  = left;
    g_sound_mixer[ch].right = right;
    spinlock_irqsave_release(&g_mixer_lock, irq_flags);

    sync_to_hardware(ch);

    return (int)size;
}

/**
 * Read callback for /sys/class/sound/controlC0/<channel>/mute.
 * Returns "0" (unmuted) or "1" (muted).
 */
static int mute_read_cb(char *buf, uint32_t max_size, void *priv)
{
    enum sound_mixer_channel ch = (enum sound_mixer_channel)(uintptr_t)priv;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_mixer_lock, &irq_flags);
    int muted = g_sound_mixer[ch].mute;
    spinlock_irqsave_release(&g_mixer_lock, irq_flags);

    int n = snprintf(buf, (size_t)max_size, "%d\n", muted);
    return (n < 0) ? 0 : n;
}

/**
 * Write callback for /sys/class/sound/controlC0/<channel>/mute.
 * "1" -> mute, "0" -> unmute.
 */
static int mute_write_cb(const char *buf, uint32_t size, void *priv)
{
    enum sound_mixer_channel ch = (enum sound_mixer_channel)(uintptr_t)priv;

    int muted = 0;
    if (size > 0 && (buf[0] == '1'))
        muted = 1;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_mixer_lock, &irq_flags);
    g_sound_mixer[ch].mute = (uint8_t)muted;
    spinlock_irqsave_release(&g_mixer_lock, irq_flags);

    sync_to_hardware(ch);

    return (int)size;
}

/* ── Initialisation ───────────────────────────────────────────────── */

void sound_core_init(void)
{
    /* If already initialised, skip */
    static int initialized = 0;
    if (initialized)
        return;
    initialized = 1;

    spinlock_init(&g_mixer_lock);

    /* Populate default mixer state */
    for (int i = 0; i < SOUND_MIXER_COUNT; i++) {
        struct sound_mixer_channel_state *st = &g_sound_mixer[i];
        st->left   = SOUND_VOLUME_DEFAULT;
        st->right  = SOUND_VOLUME_DEFAULT;
        st->mute   = 0;
        st->recsel = 0;
    }

    /* Sync from hardware if AC97 is present */
    if (ac97_present()) {
        sync_from_hardware(SOUND_MIXER_MASTER);
        sync_from_hardware(SOUND_MIXER_PCM);
        sync_from_hardware(SOUND_MIXER_MIC);
        sync_from_hardware(SOUND_MIXER_LINE_IN);
        sync_from_hardware(SOUND_MIXER_CD);

        /* Write back the defaults to ensure consistent state */
        sync_to_hardware(SOUND_MIXER_MASTER);
        sync_to_hardware(SOUND_MIXER_PCM);
    }

    /* ── Create /sys/class/sound/ hierarchy ─────────────────────────── */

    int ret = sysfs_create_dir("/sys/class");
    if (ret < 0) ret = sysfs_create_dir("/sys/class"); /* retry if exists */

    ret = sysfs_create_dir("/sys/class/sound");
    if (ret < 0) {
        kprintf("[SOUND] WARN: cannot create /sys/class/sound\n");
        return;
    }

    /* Create controlC0 directory for card 0 */
    ret = sysfs_create_dir("/sys/class/sound/controlC0");
    if (ret < 0) {
        kprintf("[SOUND] WARN: cannot create /sys/class/sound/controlC0\n");
        return;
    }

    /* Create per-channel subdirectories with volume + mute files */
    for (int i = 0; i < SOUND_MIXER_COUNT; i++) {
        char dirpath[64];
        snprintf(dirpath, sizeof(dirpath),
                 "/sys/class/sound/controlC0/%s", ch_names[i]);

        ret = sysfs_create_dir(dirpath);
        if (ret < 0) {
            kprintf("[SOUND] WARN: cannot create %s\n", dirpath);
            continue;
        }

        /* Volume control */
        char volpath[80];
        snprintf(volpath, sizeof(volpath),
                 "/sys/class/sound/controlC0/%s/volume", ch_names[i]);
        sysfs_create_writable_file(
            volpath, "75\n",
            (void *)(uintptr_t)(uintptr_t)i,
            volume_read_cb, volume_write_cb);

        /* Mute control */
        char mutepath[80];
        snprintf(mutepath, sizeof(mutepath),
                 "/sys/class/sound/controlC0/%s/mute", ch_names[i]);
        sysfs_create_writable_file(
            mutepath, "0\n",
            (void *)(uintptr_t)(uintptr_t)i,
            mute_read_cb, mute_write_cb);
    }

    kprintf("[OK] Sound core: mixer interface ready"
            " (%d channels under /sys/class/sound/controlC0/)\n",
            SOUND_MIXER_COUNT);
}

/* ── Public API ───────────────────────────────────────────────────── */

int sound_mixer_set_volume(enum sound_mixer_channel ch, uint8_t left, uint8_t right)
{
    if (ch < 0 || ch >= SOUND_MIXER_COUNT)
        return -1;

    if (left  > SOUND_VOLUME_MAX) left  = SOUND_VOLUME_MAX;
    if (right > SOUND_VOLUME_MAX) right = SOUND_VOLUME_MAX;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_mixer_lock, &irq_flags);
    g_sound_mixer[ch].left  = left;
    g_sound_mixer[ch].right = right;
    spinlock_irqsave_release(&g_mixer_lock, irq_flags);

    sync_to_hardware(ch);
    return 0;
}

int sound_mixer_set_mute(enum sound_mixer_channel ch, int mute)
{
    if (ch < 0 || ch >= SOUND_MIXER_COUNT)
        return -1;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_mixer_lock, &irq_flags);
    g_sound_mixer[ch].mute = mute ? 1 : 0;
    spinlock_irqsave_release(&g_mixer_lock, irq_flags);

    sync_to_hardware(ch);
    return 0;
}

int sound_mixer_set_recsel(enum sound_mixer_channel ch, int select)
{
    if (ch < 0 || ch >= SOUND_MIXER_COUNT)
        return -1;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_mixer_lock, &irq_flags);
    g_sound_mixer[ch].recsel = select ? 1 : 0;
    spinlock_irqsave_release(&g_mixer_lock, irq_flags);

    return 0;
}

uint16_t sound_mixer_read(enum sound_mixer_channel ch)
{
    if (ch < 0 || ch >= SOUND_MIXER_COUNT)
        return 0;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_mixer_lock, &irq_flags);
    uint16_t val = (uint16_t)((uint16_t)g_sound_mixer[ch].left |
                              ((uint16_t)g_sound_mixer[ch].right << 8));
    if (g_sound_mixer[ch].mute)
        val |= 0x8000;
    spinlock_irqsave_release(&g_mixer_lock, irq_flags);

    return val;
}

int sound_mixer_write(enum sound_mixer_channel ch, uint16_t val)
{
    if (ch < 0 || ch >= SOUND_MIXER_COUNT)
        return -1;

    uint8_t left  = (uint8_t)(val & 0x00FF);
    uint8_t right = (uint8_t)((val >> 8) & 0x00FF);
    int     mute  = (val & 0x8000) ? 1 : 0;

    if (left  > SOUND_VOLUME_MAX) left  = SOUND_VOLUME_MAX;
    if (right > SOUND_VOLUME_MAX) right = SOUND_VOLUME_MAX;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_mixer_lock, &irq_flags);
    g_sound_mixer[ch].left  = left;
    g_sound_mixer[ch].right = right;
    g_sound_mixer[ch].mute  = (uint8_t)mute;
    spinlock_irqsave_release(&g_mixer_lock, irq_flags);

    sync_to_hardware(ch);
    return 0;
}

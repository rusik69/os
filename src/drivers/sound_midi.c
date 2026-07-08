/*
 * sound_midi.c — OSS MIDI sequencer (/dev/sequencer)
 *
 * Provides a basic MIDI sequencer interface that accepts timed MIDI
 * events (note-on/note-off, velocity, program change, control change)
 * and plays them through the PC speaker (or AC97 if available).
 *
 * Implements a simple timed event queue.  Events are dequeued by a
 * background timer tick (typically 10 ms resolution).
 *
 * Item S12 — OSS MIDI sequencer device
 */

#include "speaker.h"
#include "ac97.h"
#include "devfs.h"
#include "string.h"
#include "printf.h"
#include "spinlock.h"
#include "heap.h"
#include "timer.h"
#include "errno.h"
#include "fm_synth.h"

/* ── MIDI sequencer constants ────────────────────────────────────── */

#define MIDI_SEQ_MAX_QUEUE   256   /* max queued events */
#define MIDI_SEQ_MAX_VOICES   16   /* polyphony voices */
#define MIDI_NUM_CHANNELS     16   /* MIDI channels */

/* MIDI status byte masks */
#define MIDI_STATUS_NOTE_OFF       0x80
#define MIDI_STATUS_NOTE_ON        0x90
#define MIDI_STATUS_POLY_PRESSURE  0xA0
#define MIDI_STATUS_CONTROL_CHANGE 0xB0
#define MIDI_STATUS_PROGRAM_CHANGE 0xC0
#define MIDI_STATUS_CHAN_PRESSURE  0xD0
#define MIDI_STATUS_PITCH_BEND     0xE0

/* ── Sequencer event ─────────────────────────────────────────────── */

struct midi_seq_event {
    uint64_t  tick;        /* absolute tick to fire */
    uint8_t   status;      /* MIDI status byte (includes channel) */
    uint8_t   data1;       /* first data byte */
    uint8_t   data2;       /* second data byte (velocity/value) */
    uint8_t   in_use;      /* 1 if slot occupied */
};

/* ── Voice state (per-note tracking) ─────────────────────────────── */

struct midi_voice {
    uint8_t   note;        /* MIDI note number */
    uint8_t   velocity;    /* current velocity */
    uint8_t   channel;     /* MIDI channel */
    uint8_t   active;      /* 1 if note is sounding */
    uint64_t  start_tick;  /* tick when note started */
};

/* ── MIDI channel state (program, controllers, pitch bend) ───────── */

struct midi_channel {
    uint8_t   program;     /* current program number (0-127) */
    uint8_t   volume;      /* channel volume (CC 7) */
    uint8_t   pan;         /* pan (CC 10): 0=left, 64=center, 127=right */
    uint16_t  pitch_bend;  /* pitch bend value (0-16383, 8192=center) */
    uint8_t   modulation;  /* modulation wheel (CC 1) */
    uint8_t   sustain;     /* sustain pedal (CC 64): 0=off, 127=on */
};

/* ── Global sequencer state ──────────────────────────────────────── */

static struct midi_seq_event g_seq_queue[MIDI_SEQ_MAX_QUEUE];
static int g_seq_count = 0;              /* number of queued events */
static int g_seq_head = 0;               /* dequeue index */
static int g_seq_tail = 0;               /* enqueue index */

static struct midi_voice    g_voices[MIDI_SEQ_MAX_VOICES];
static struct midi_channel  g_channels[MIDI_NUM_CHANNELS];

static uint64_t g_seq_cur_tick = 0;      /* current sequencer tick */
static int      g_seq_running = 0;       /* 1 if sequencer active */
static int      g_seq_initialized = 0;
static int      g_seq_tempo = 500000;    /* microseconds per quarter note */
static int      g_seq_ppqn = 96;         /* pulses per quarter note */

static spinlock_t g_seq_lock;

/* Auto-advance timer: number of ticks per sequencer tick */
static uint64_t g_seq_timer_interval = 0;

/* ── Prototypes ──────────────────────────────────────────────────── */

static void midi_note_on(uint8_t channel, uint8_t note, uint8_t velocity);
static void midi_note_off(uint8_t channel, uint8_t note, uint8_t velocity);
static void midi_program_change(uint8_t channel, uint8_t program);
static void midi_control_change(uint8_t channel, uint8_t controller, uint8_t value);
static void midi_pitch_bend(uint8_t channel, uint16_t bend);

/* ── Sequencer timer callback ────────────────────────────────────── */

static void midi_seq_tick_handler(void)
{
    if (!g_seq_initialized || !g_seq_running)
        return;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_seq_lock, &irq_flags);

    g_seq_cur_tick++;

    /* Process all events scheduled to fire at or before current tick */
    while (g_seq_count > 0) {
        struct midi_seq_event *ev = &g_seq_queue[g_seq_head];
        if (ev->tick > g_seq_cur_tick)
            break;

        /* Dispatch event */
        uint8_t chan = ev->status & 0x0F;
        uint8_t msg  = ev->status & 0xF0;

        switch (msg) {
        case MIDI_STATUS_NOTE_OFF:
            midi_note_off(chan, ev->data1, ev->data2);
            break;
        case MIDI_STATUS_NOTE_ON:
            if (ev->data2 == 0) {
                /* Note-on with velocity 0 = note-off */
                midi_note_off(chan, ev->data1, 0);
            } else {
                midi_note_on(chan, ev->data1, ev->data2);
            }
            break;
        case MIDI_STATUS_CONTROL_CHANGE:
            midi_control_change(chan, ev->data1, ev->data2);
            break;
        case MIDI_STATUS_PROGRAM_CHANGE:
            midi_program_change(chan, ev->data1);
            break;
        case MIDI_STATUS_PITCH_BEND:
            midi_pitch_bend(chan, (uint16_t)(ev->data1) | ((uint16_t)(ev->data2) << 7));
            break;
        default:
            /* Unsupported message type — ignored */
            break;
        }

        /* Mark slot empty */
        ev->in_use = 0;
        g_seq_head = (g_seq_head + 1) % MIDI_SEQ_MAX_QUEUE;
        g_seq_count--;
    }

    spinlock_irqsave_release(&g_seq_lock, irq_flags);
}

/* ── Timer registration ──────────────────────────────────────────── */

/* We use a simple tick hook.  The timer subsystem calls this at
 * TIMER_FREQ / 100 Hz (approximately 10 ms resolution). */
static void midi_seq_timer_tick(void)
{
    if (g_seq_initialized && g_seq_running)
        midi_seq_tick_handler();
}

/* ── Voice allocation (simple steal) ─────────────────────────────── */

static int midi_alloc_voice(uint8_t channel, uint8_t note)
{
    /* First, look for an inactive voice on the same channel */
    for (int i = 0; i < MIDI_SEQ_MAX_VOICES; i++) {
        if (!g_voices[i].active) {
            g_voices[i].channel = channel;
            g_voices[i].note = note;
            g_voices[i].active = 1;
            g_voices[i].start_tick = g_seq_cur_tick;
            return i;
        }
    }

    /* All voices busy — steal the oldest voice */
    uint64_t oldest_tick = g_voices[0].start_tick;
    int oldest_idx = 0;
    for (int i = 1; i < MIDI_SEQ_MAX_VOICES; i++) {
        if (g_voices[i].start_tick < oldest_tick) {
            oldest_tick = g_voices[i].start_tick;
            oldest_idx = i;
        }
    }
    g_voices[oldest_idx].channel = channel;
    g_voices[oldest_idx].note = note;
    g_voices[oldest_idx].active = 1;
    g_voices[oldest_idx].start_tick = g_seq_cur_tick;
    return oldest_idx;
}

static void midi_free_voice(int idx)
{
    if (idx >= 0 && idx < MIDI_SEQ_MAX_VOICES)
        g_voices[idx].active = 0;
}

static int midi_find_voice(uint8_t channel, uint8_t note)
{
    for (int i = 0; i < MIDI_SEQ_MAX_VOICES; i++) {
        if (g_voices[i].active &&
            g_voices[i].channel == channel &&
            g_voices[i].note == note)
            return i;
    }
    return -1;
}

/* ── MIDI event handlers ─────────────────────────────────────────── */

static void midi_note_on(uint8_t channel, uint8_t note, uint8_t velocity)
{
    if (channel >= MIDI_NUM_CHANNELS || note > 127)
        return;

    /* Apply channel volume scaling */
    uint32_t scaled_vol = (uint32_t)velocity * g_channels[channel].volume / 127;

    /* Allocate voice */
    int voice = midi_alloc_voice(channel, note);
    if (voice < 0) return;

    g_voices[voice].velocity = (uint8_t)scaled_vol;

    /* Play note through FM synthesiser */
    fm_synth_note_on(channel, note, (uint8_t)scaled_vol);
}

static void midi_note_off(uint8_t channel, uint8_t note, uint8_t velocity)
{
    (void)velocity;
    if (channel >= MIDI_NUM_CHANNELS || note > 127)
        return;

    /* Check sustain pedal */
    if (g_channels[channel].sustain >= 64) {
        /* Sustain is on — don't release note yet.
         * In a full implementation, mark note for release when
         * sustain pedal is released. */
        return;
    }

    int voice = midi_find_voice(channel, note);
    if (voice < 0) return;

    midi_free_voice(voice);
    fm_synth_note_off(channel, note);
}

static void midi_program_change(uint8_t channel, uint8_t program)
{
    if (channel >= MIDI_NUM_CHANNELS)
        return;
    g_channels[channel].program = program;
}

static void midi_control_change(uint8_t channel, uint8_t controller, uint8_t value)
{
    if (channel >= MIDI_NUM_CHANNELS)
        return;

    switch (controller) {
    case 1:   /* Modulation wheel */
        g_channels[channel].modulation = value;
        break;
    case 7:   /* Channel volume */
        g_channels[channel].volume = value;
        break;
    case 10:  /* Pan */
        g_channels[channel].pan = value;
        break;
    case 64:  /* Sustain pedal */
        g_channels[channel].sustain = value;
        /* If sustain released, release all sustained notes */
        if (value < 64) {
            for (int i = 0; i < MIDI_SEQ_MAX_VOICES; i++) {
                if (g_voices[i].active && g_voices[i].channel == channel) {
                    /* In a full impl, schedule release with release velocity */
                }
            }
        }
        break;
    case 120: /* All sounds off */
    case 123: /* All notes off */
        for (int i = 0; i < MIDI_SEQ_MAX_VOICES; i++) {
            if (g_voices[i].active && g_voices[i].channel == channel)
                g_voices[i].active = 0;
        }
        fm_synth_all_notes_off();
        break;
    default:
        break;
    }
}

static void midi_pitch_bend(uint8_t channel, uint16_t bend)
{
    if (channel >= MIDI_NUM_CHANNELS)
        return;
    g_channels[channel].pitch_bend = bend;
}

/* ── Public API: queue events ────────────────────────────────────── */

static int midi_seq_queue_event(uint8_t status, uint8_t data1, uint8_t data2,
                          uint32_t delta_ticks)
{
    if (!g_seq_initialized)
        return -ENODEV;

    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_seq_lock, &irq_flags);

    if (g_seq_count >= MIDI_SEQ_MAX_QUEUE) {
        spinlock_irqsave_release(&g_seq_lock, irq_flags);
        return -ENOSPC;
    }

    struct midi_seq_event *ev = &g_seq_queue[g_seq_tail];
    ev->tick   = g_seq_cur_tick + delta_ticks;
    ev->status = status;
    ev->data1  = data1;
    ev->data2  = data2;
    ev->in_use = 1;

    g_seq_tail = (g_seq_tail + 1) % MIDI_SEQ_MAX_QUEUE;
    g_seq_count++;

    spinlock_irqsave_release(&g_seq_lock, irq_flags);
    return 0;
}

int midi_seq_note_on(uint8_t channel, uint8_t note, uint8_t velocity)
{
    return midi_seq_queue_event(MIDI_STATUS_NOTE_ON | (channel & 0x0F),
                                 note, velocity, 0);
}

static int midi_seq_note_off(uint8_t channel, uint8_t note, uint8_t velocity)
{
    return midi_seq_queue_event(MIDI_STATUS_NOTE_OFF | (channel & 0x0F),
                                 note, velocity, 0);
}

static int midi_seq_program_change(uint8_t channel, uint8_t program)
{
    return midi_seq_queue_event(MIDI_STATUS_PROGRAM_CHANGE | (channel & 0x0F),
                                 program, 0, 0);
}

static int midi_seq_control_change(uint8_t channel, uint8_t controller, uint8_t value)
{
    return midi_seq_queue_event(MIDI_STATUS_CONTROL_CHANGE | (channel & 0x0F),
                                 controller, value, 0);
}

/* ── devfs callbacks for /dev/sequencer ──────────────────────────── */

/*
 * Raw MIDI byte stream format for /dev/sequencer:
 *   The standard OSS sequencer device writes raw MIDI events as
 *   (status, data1, data2) triples (or pairs for 2-byte messages).
 *   We accept a simple raw-byte protocol:
 *     - If byte has MSB set: it's a status byte.
 *       Next byte(s) are data (1 or 2 depending on status type).
 *     - Running status is supported (status remembered).
 */

static uint8_t g_running_status = 0;
static int     g_seq_bytes_pending = 0;
static uint8_t g_seq_pending_data[2];

static int seq_write(void *priv, const void *data, uint32_t size)
{
    (void)priv;
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t consumed = 0;

    while (consumed < size) {
        uint8_t b = bytes[consumed++];

        if (b & 0x80) {
            /* Status byte */
            g_running_status = b;
            g_seq_bytes_pending = 0;
            continue;
        }

        /* Data byte (uses running status) */
        if (g_running_status == 0)
            continue;  /* no running status — ignore */

        g_seq_pending_data[g_seq_bytes_pending++] = b;

        uint8_t msg = g_running_status & 0xF0;
        int needed = 0;

        switch (msg) {
        case MIDI_STATUS_NOTE_OFF:
        case MIDI_STATUS_NOTE_ON:
        case MIDI_STATUS_POLY_PRESSURE:
        case MIDI_STATUS_CONTROL_CHANGE:
        case MIDI_STATUS_PITCH_BEND:
            needed = 2;
            break;
        case MIDI_STATUS_PROGRAM_CHANGE:
        case MIDI_STATUS_CHAN_PRESSURE:
            needed = 1;
            break;
        default:
            needed = 1;
            break;
        }

        if (g_seq_bytes_pending >= needed) {
            midi_seq_queue_event(g_running_status,
                                  g_seq_pending_data[0],
                                  needed > 1 ? g_seq_pending_data[1] : 0, 0);
            g_seq_bytes_pending = 0;
        }
    }

    return (int)size;
}

static int seq_read(void *priv, void *buf, uint32_t max_size, uint32_t *out_size)
{
    (void)priv;
    if (!buf || max_size < 4) {
        if (out_size) *out_size = 0;
        return 0;
    }

    /* Return sequencer position and state info */
    uint8_t *out = (uint8_t *)buf;
    uint64_t irq_flags;
    spinlock_irqsave_acquire(&g_seq_lock, &irq_flags);

    /* Format: current tick (8 bytes) + tempo (4 bytes) + running (1 byte) */
    uint64_t tick = g_seq_cur_tick;
    uint32_t tempo = (uint32_t)g_seq_tempo;

    spinlock_irqsave_release(&g_seq_lock, irq_flags);

    int pos = 0;
    out[pos++] = (uint8_t)(tick >> 0);
    out[pos++] = (uint8_t)(tick >> 8);
    out[pos++] = (uint8_t)(tick >> 16);
    out[pos++] = (uint8_t)(tick >> 24);
    if (pos + 4 <= (int)max_size) {
        out[pos++] = (uint8_t)(tick >> 32);
        out[pos++] = (uint8_t)(tick >> 40);
        out[pos++] = (uint8_t)(tick >> 48);
        out[pos++] = (uint8_t)(tick >> 56);
    }
    if (pos + 4 <= (int)max_size) {
        out[pos++] = (uint8_t)(tempo >> 0);
        out[pos++] = (uint8_t)(tempo >> 8);
        out[pos++] = (uint8_t)(tempo >> 16);
        out[pos++] = (uint8_t)(tempo >> 24);
    }
    if (pos < (int)max_size)
        out[pos++] = g_seq_running ? 1 : 0;

    if (out_size) *out_size = (uint32_t)pos;
    return 0;
}

/* ── Sequencer tempo / start / stop ──────────────────────────────── */

static int midi_seq_start(void)
{
    if (!g_seq_initialized) return -ENODEV;
    g_seq_running = 1;
    g_seq_cur_tick = 0;
    return 0;
}

static int midi_seq_stop(void)
{
    g_seq_running = 0;
    /* All notes off */
    for (int i = 0; i < MIDI_SEQ_MAX_VOICES; i++)
        g_voices[i].active = 0;
    fm_synth_all_notes_off();
    return 0;
}

static int midi_seq_set_tempo(uint32_t us_per_qn)
{
    if (us_per_qn < 10000 || us_per_qn > 10000000)
        return -EINVAL;
    g_seq_tempo = us_per_qn;
    return 0;
}

/* ── Initialization ──────────────────────────────────────────────── */

static void sound_midi_init(void)
{
    if (g_seq_initialized)
        return;

    /* Initialize FM synthesis engine for MIDI audio output */
    fm_synth_init(FM_SYNTH_DEFAULT_RATE);

    spinlock_init(&g_seq_lock);

    memset(g_seq_queue, 0, sizeof(g_seq_queue));
    memset(g_voices, 0, sizeof(g_voices));
    memset(g_channels, 0, sizeof(g_channels));

    /* Initialize default channel state */
    for (int i = 0; i < MIDI_NUM_CHANNELS; i++) {
        g_channels[i].volume = 100;     /* default volume */
        g_channels[i].pan = 64;         /* center */
        g_channels[i].pitch_bend = 8192; /* center (no bend) */
    }

    g_seq_cur_tick = 0;
    g_seq_running = 1;
    g_seq_tempo = 500000;  /* 120 BPM */
    g_seq_ppqn = 96;
    g_seq_head = 0;
    g_seq_tail = 0;
    g_seq_count = 0;

    g_seq_initialized = 1;

    /* Register devfs device */
    int ret = devfs_register_device("sequencer", NULL, seq_read, seq_write);
    if (ret == 0) {
        kprintf("[OK] MIDI: /dev/sequencer registered\n");
    } else {
        kprintf("[MIDI] WARN: failed to register /dev/sequencer\n");
    }

    kprintf("[OK] MIDI sequencer initialized (%d voices, %d queue slots)\n",
            MIDI_SEQ_MAX_VOICES, MIDI_SEQ_MAX_QUEUE);
}
#include "module.h"

/* ── Raw MIDI device open/close/read/write ──────────────────────────── */

/*
 * Raw MIDI device (/dev/midi) interface.
 * Provides direct MIDI byte stream access through the sequencer.
 *
 * The raw MIDI device allows a simpler byte-stream interface where
 * MIDI messages are written/read as raw bytes without timing info.
 */

static int g_midi_dev_open_count = 0;  /* How many times raw MIDI is opened */

/**
 * midi_raw_open — Open the raw MIDI device.
 *
 * Initializes per-open state for raw MIDI access.
 * Returns 0 on success.
 */
static int midi_raw_open(void)
{
    if (!g_seq_initialized)
        return -ENODEV;

    if (g_midi_dev_open_count == 0) {
        /* First open: reset running status */
        g_running_status = 0;
        g_seq_bytes_pending = 0;
        kprintf("[MIDI] Raw MIDI device opened\n");
    }

    g_midi_dev_open_count++;
    return 0;
}

/**
 * midi_raw_close — Close the raw MIDI device.
 *
 * All notes off and release resources.
 * Returns 0 on success.
 */
static int midi_raw_close(void)
{
    if (!g_seq_initialized)
        return -ENODEV;

    if (g_midi_dev_open_count > 0)
        g_midi_dev_open_count--;

    if (g_midi_dev_open_count == 0) {
        /* Last close: all notes off */
        midi_seq_stop();
        g_running_status = 0;
        g_seq_bytes_pending = 0;
        kprintf("[MIDI] Raw MIDI device closed, all notes off\n");
    }

    return 0;
}

/**
 * midi_raw_write — Write raw MIDI bytes to the sequencer.
 *
 * Accepts a stream of raw MIDI bytes.  Supports running status.
 *
 * @data:  Pointer to MIDI byte data
 * @size:  Number of bytes to write
 *
 * Returns number of bytes written, or negative on error.
 */
static int midi_raw_write(const uint8_t *data, uint32_t size)
{
    if (!g_seq_initialized)
        return -ENODEV;
    if (!data || size == 0)
        return -EINVAL;

    /* Use the existing seq_write handler for byte stream parsing */
    return seq_write(NULL, data, size);
}

/**
 * midi_raw_read — Read raw MIDI bytes from the sequencer.
 *
 * Returns current sequencer state as raw bytes.
 *
 * @buf:      Output buffer
 * @max_size: Maximum bytes to read
 * @out_size: Actual bytes read
 *
 * Returns 0 on success.
 */
static int midi_raw_read(uint8_t *buf, uint32_t max_size, uint32_t *out_size)
{
    if (!g_seq_initialized)
        return -ENODEV;
    if (!buf || max_size == 0) {
        if (out_size) *out_size = 0;
        return -EINVAL;
    }

    return seq_read(NULL, buf, max_size, out_size);
}

/**
 * midi_raw_ioctl — Handle raw MIDI device ioctls.
 *
 * Supports basic MIDI control operations.
 *
 * @cmd:  IOCTL command
 * @arg:  IOCTL argument
 *
 * Returns 0 on success, negative on error.
 */
static int midi_raw_ioctl(int cmd, void *arg)
{
    (void)arg;

    switch (cmd) {
    case 0x01: /* MIDI_RESET — reset all channels */
        for (int i = 0; i < MIDI_NUM_CHANNELS; i++) {
            g_channels[i].volume = 100;
            g_channels[i].pan = 64;
            g_channels[i].pitch_bend = 8192;
            g_channels[i].modulation = 0;
            g_channels[i].sustain = 0;
        }
        for (int i = 0; i < MIDI_SEQ_MAX_VOICES; i++)
            g_voices[i].active = 0;
        fm_synth_all_notes_off();
        return 0;

    default:
        return -EINVAL;
    }
}

module_init(sound_midi_init);

/* ── Stub: midi_open ─────────────────────────────── */
static int midi_open(void *file)
{
    (void)file;
    kprintf("[midi] midi_open: not yet implemented\n");
    return 0;
}
/* ── Stub: midi_close ─────────────────────────────── */
static int midi_close(void *file)
{
    (void)file;
    kprintf("[midi] midi_close: not yet implemented\n");
    return 0;
}
/* ── Stub: midi_write ─────────────────────────────── */
static int midi_write(void *file, const void *buf, size_t count)
{
    (void)file;
    (void)buf;
    (void)count;
    kprintf("[midi] midi_write: not yet implemented\n");
    return 0;
}
/* ── Stub: midi_ioctl ─────────────────────────────── */
static int midi_ioctl(void *file, int cmd, void *arg)
{
    (void)file;
    (void)cmd;
    (void)arg;
    kprintf("[midi] midi_ioctl: not yet implemented\n");
    return 0;
}

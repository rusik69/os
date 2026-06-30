/*
 * sound_oss.h — OSS /dev/dsp audio interface definitions
 *
 * Provides the ioctl command codes, constants, and function declarations
 * for the Open Sound System (OSS) /dev/dsp character device.
 *
 * OSS DSP ioctls (SNDCTL_DSP_*) control sample format, rate, channels,
 * fragment size, and buffer queries.  OSS mixer ioctls (SOUND_MIXER_*)
 * control volume, mute, and recording source selection.
 *
 * Task #8 — D142 OSS /dev/dsp implementation (open, read, write, ioctl)
 */

#ifndef SOUND_OSS_H
#define SOUND_OSS_H

#include "types.h"

/* ── Audio format codes (AFMT_*) ───────────────────────────────────── */
#define AFMT_U8        8       /* 8-bit unsigned PCM */
#define AFMT_S16_LE    16      /* 16-bit signed little-endian PCM */
#define AFMT_S16_BE    17      /* 16-bit signed big-endian PCM */
#define AFMT_QUERY     0       /* Query for the current format */

/* Format capability bitmask for SNDCTL_DSP_GETFMTS */
#define AFMT_BIT_U8      (1U << 0)   /* bit for AFMT_U8 */
#define AFMT_BIT_S16_LE  (1U << 1)   /* bit for AFMT_S16_LE */
#define AFMT_BIT_S16_BE  (1U << 2)   /* bit for AFMT_S16_BE */

/* ── PCM trigger bits ─────────────────────────────────────────────── */
#define PCM_ENABLE_INPUT   0x00000001
#define PCM_ENABLE_OUTPUT  0x00000002

/* ── OSS DSP ioctl commands (SNDCTL_DSP_*) ─────────────────────────── */
#define SNDCTL_DSP_RESET       0x00500000  /* Reset DSP (flush buffers) */
#define SNDCTL_DSP_SYNC        0x00500001  /* Sync (drain and wait) */
#define SNDCTL_DSP_SPEED       0x00500002  /* Set sample rate */
#define SNDCTL_DSP_GETBLKSIZE  0x00500004  /* Get block size */
#define SNDCTL_DSP_SETFMT      0x00500005  /* Set sample format */
#define SNDCTL_DSP_CHANNELS    0x00500006  /* Set mono(1)/stereo(2) */
#define SNDCTL_DSP_GETTRIGGER  0x00500011  /* Get trigger state */
#define SNDCTL_DSP_SETTRIGGER  0x00500010  /* Set trigger state */
#define SNDCTL_DSP_GETOSPACE   0x0050000C  /* Get output buffer info */
#define SNDCTL_DSP_GETISPACE   0x0050000D  /* Get input buffer info */
#define SNDCTL_DSP_SETFRAGMENT 0x0050000A  /* Set fragment size/count */
#define SNDCTL_DSP_GETCAPS     0x0050000F  /* Get driver capabilities */
#define SNDCTL_DSP_POST        0x00500008  /* Post output (fire DMA) */
#define SNDCTL_DSP_GETIPTR     0x00500015  /* Get input pointer */
#define SNDCTL_DSP_GETOPTR     0x00500014  /* Get output pointer */
#define SNDCTL_DSP_SETRECORD_SOURCE  0x00500050  /* Set record source */
#define SNDCTL_DSP_GETRECORD_SOURCE  0x00500051  /* Get record source */
#define SNDCTL_DSP_SETRECORD_GAIN    0x00500052  /* Set record gain */
#define SNDCTL_DSP_GETRECORD_GAIN    0x00500053  /* Get record gain */

/* ── Additional standard OSS SNDCTL_DSP_* ioctls ──────────────────── */
#define SNDCTL_DSP_SUBDIVIDE        0x00500007  /* Set/query subsample divide */
#define SNDCTL_DSP_GETFMTS          0x00500012  /* Get supported format bitmask */
#define SNDCTL_DSP_GETODELAY        0x00500016  /* Get output delay in bytes */
#define SNDCTL_DSP_GETCHANNELS      0x00500017  /* Get actual channel count */
#define SNDCTL_DSP_SETPLAYVOL       0x00500018  /* Set playback volume */
#define SNDCTL_DSP_GETPLAYVOL       0x00500019  /* Get playback volume */
#define SNDCTL_DSP_SETRECVOL        0x0050001A  /* Set recording volume */
#define SNDCTL_DSP_GETRECVOL        0x0050001B  /* Get recording volume */
#define SNDCTL_DSP_PROFILE          0x0050001C  /* Set/get DSP profile */
#define SNDCTL_DSP_GETERROR         0x0050001D  /* Get error statistics */

/* ── OSS Mixer ioctl commands (SOUND_MIXER_*) ──────────────────────── */
#define SOUND_MIXER_READ_VOLUME   0x80044D00  /* Read master volume */
#define SOUND_MIXER_WRITE_VOLUME  0xC0044D00  /* Write master volume */
#define SOUND_MIXER_READ_MUTE     0x80044D01  /* Read master mute */
#define SOUND_MIXER_WRITE_MUTE    0xC0044D01  /* Write master mute */
#define SOUND_MIXER_READ_RECMASK  0x80044D02  /* Read record mask */
#define SOUND_MIXER_READ_DEVMASK  0x80044D03  /* Read device mask */
#define SOUND_MIXER_READ_RECSRC   0x80044D04  /* Read record source */
#define SOUND_MIXER_WRITE_RECSRC  0xC0044D04  /* Write record source */
#define SOUND_MIXER_READ_STEREO   0x80044D05  /* Read stereo support */
#define SOUND_MIXER_READ_CAPS     0x80044D06  /* Read capabilities */

/* ── OSS audio buffer info (for GETOSPACE/GETISPACE) ──────────────── */
struct audio_buf_info {
    int fragments;      /**< Number of fragments available */
    int fragstotal;     /**< Total number of fragments */
    int fragsize;       /**< Size of each fragment in bytes */
    int bytes;          /**< Total bytes available */
};

/* ── OSS count/pointer info (for GETOPTR/GETIPTR) ──────────────────── */
struct count_info {
    int bytes;          /**< Total bytes transferred */
    int blocks;         /**< Total blocks/fragments transferred */
    int ptr;            /**< Current DMA pointer (in bytes) */
};

/* ── Driver capability bits ─────────────────────────────────────────── */
#define DSP_CAP_REVISION    0x00000001  /* Supports revision ioctls */
#define DSP_CAP_DUPLEX      0x00000002  /* Full-duplex support */
#define DSP_CAP_REALTIME    0x00000004  /* Real-time support */
#define DSP_CAP_BATCH       0x00000008  /* Batched writes */
#define DSP_CAP_COPROC      0x00000010  /* Has coprocessor */
#define DSP_CAP_TRIGGER     0x00000020  /* Supports SETTRIGGER */
#define DSP_CAP_MMAP        0x00000040  /* Supports mmap */

/* ── OSS error info structure (for SNDCTL_DSP_GETERROR) ───────────── */
struct audio_errinfo {
    int play_underruns;         /**< Number of playback underruns */
    int rec_overruns;           /**< Number of capture overruns */
    unsigned long play_ptradjust;  /**< Playback pointer adjustments */
    unsigned long rec_ptradjust;   /**< Record pointer adjustments */
    int play_error_count;       /**< Playback error count */
    int rec_error_count;        /**< Record error count */
    int play_lost_interrupts;   /**< Playback lost interrupts */
    int rec_lost_interrupts;    /**< Record lost interrupts */
    int play_last_error;        /**< Last playback error code */
    int rec_last_error;         /**< Last record error code */
};

/* ── DSP profile constants ──────────────────────────────────────────── */
#define DSP_PROFILE_DEFAULT     0   /**< Default profile */
#define DSP_PROFILE_LOW_LATENCY 1   /**< Low-latency profile */
#define DSP_PROFILE_HIGH_Q      2   /**< High-quality profile */

/* ── Function declarations ─────────────────────────────────────────── */

/**
 * sound_oss_ioctl — Handle OSS DSP and mixer ioctl commands.
 *
 * @cmd:  IOCTL command code (SNDCTL_DSP_* or SOUND_MIXER_*).
 * @arg:  Userspace pointer argument for the ioctl (raw uint64_t from syscall).
 *
 * The function handles userspace read/write via copy_from_user/copy_to_user
 * internally based on the ioctl direction encoded in @cmd.
 *
 * Returns 0 on success, or a negative errno on failure.
 * This function is called from the sys_ioctl dispatch layer.
 */
int sound_oss_ioctl(int cmd, uint64_t arg);

#endif /* SOUND_OSS_H */

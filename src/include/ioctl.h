/*
 * ioctl.h — ioctl command constants and dispatch declarations
 *
 * Linux-compatible ioctl command codes and the kernel's sys_ioctl
 * entry point.  Categorised by subsystem:
 *   - FD-level:  FIOCLEX, FIONCLEX, FIONBIO, FIOASYNC (on fd flags)
 *   - Terminal:  TIOCGWINSZ, TIOCSWINSZ
 *   - Socket:    SIOCGIFNAME, SIOCGIFINDEX, SIOCGIFHWADDR,
 *                SIOCGIFFLAGS, SIOCSIFFLAGS
 *   - Block:     SG_IO (SCSI generic passthrough)
 */

#ifndef IOCTL_H
#define IOCTL_H

#include "types.h"

/* ── ioctl direction/type macros (Linux-compatible) ───────────────── */

#define _IOC_NONE      0U
#define _IOC_WRITE     1U
#define _IOC_READ      2U

#define _IOC(dir, type, nr, size) \
    (((uint32_t)((dir)  & 3U) << 30) | \
     ((uint32_t)(type) & 0xFFU) << 16  | \
     ((uint32_t)(nr)   & 0xFFU) << 8   | \
     ((uint32_t)(size) & 0x3FFFU))

#define _IO(type, nr)          _IOC(_IOC_NONE,  (type), (nr), 0U)
#define _IOR(type, nr, size)   _IOC(_IOC_READ,  (type), (nr), (uint32_t)sizeof(size))
#define _IOW(type, nr, size)   _IOC(_IOC_WRITE, (type), (nr), (uint32_t)sizeof(size))
#define _IOWR(type, nr, size)  _IOC(_IOC_READ | _IOC_WRITE, (type), (nr), (uint32_t)sizeof(size))

/* Extract fields from an ioctl command (for generic dispatch) */
#define _IOC_DIR(cmd)  (((uint32_t)(cmd) >> 30) & 3U)
#define _IOC_TYPE(cmd) (((uint32_t)(cmd) >> 16) & 0xFFU)
#define _IOC_NR(cmd)   (((uint32_t)(cmd) >> 8)  & 0xFFU)
#define _IOC_SIZE(cmd) ((uint32_t)(cmd) & 0x3FFFU)

/* ── File-descriptor-level ioctls ─────────────────────────────────── */

#define FIONCLEX   0x5450      /* clear close-on-exec flag */
#define FIOCLEX    0x5451      /* set close-on-exec flag */
#define FIONBIO    0x5421      /* set/clear O_NONBLOCK (arg: int*) */
#define FIOASYNC   0x5452      /* set/clear O_ASYNC (arg: int*) */
#define FIOQSIZE   0x5460      /* get file size (return uint64_t) */

/* ── Terminal ioctls (TIOC*) ──────────────────────────────────────── */

#define TIOCGWINSZ  0x5413     /* get terminal window size */
#define TIOCSWINSZ  0x5414     /* set terminal window size */

/* ── Network interface ioctls (SIOCGIF*) ──────────────────────────── */

#define SIOCGIFNAME     0x8910  /* get interface name (arg: struct ifreq) */
#define SIOCGIFINDEX    0x8933  /* get interface index (arg: struct ifreq) */
#define SIOCGIFFLAGS    0x8913  /* get interface flags (arg: struct ifreq) */
#define SIOCSIFFLAGS    0x8914  /* set interface flags (arg: struct ifreq) */
#define SIOCGIFHWADDR   0x8927  /* get hardware (MAC) address */

/* ── Block-device ioctls ──────────────────────────────────────────── */

#define SG_IO        0x2285    /* SCSI generic passthrough */
#define SG_GET_VERSION_NUM  0x2282  /* get SG driver version */
#define SG_SET_TIMEOUT      0x2283  /* set SG timeout */
#define SG_GET_TIMEOUT      0x2284  /* get SG timeout */

/* ── Syscall entry point (implemented in sys_ioctl.c) ─────────────── */

uint64_t sys_ioctl(uint64_t fd, uint64_t cmd, uint64_t arg);

#endif /* IOCTL_H */

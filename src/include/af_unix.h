/*
 * af_unix.h — AF_UNIX (UNIX domain) socket definitions
 *
 * Defines UNIX domain socket address structures, ancillary data types,
 * and constants for SOCK_STREAM and SOCK_DGRAM communication over
 * AF_UNIX (domain 1).
 *
 * Reference: Linux <linux/un.h>, <linux/unistd.h>, POSIX.1-2001
 */

#ifndef AF_UNIX_H
#define AF_UNIX_H

#include "types.h"

/* ── Address family constant ──────────────────────────────────────── */
#define AF_UNIX             1
#define PF_UNIX             1

/* ── Socket types ─────────────────────────────────────────────────── */
/* AF_UNIX supports both stream (connection-oriented) and datagram
 * (connectionless, message-boundary-preserving) modes. */
/* SOCK_STREAM = 1, SOCK_DGRAM = 2  (from socket.h) */

/* ── Path length ──────────────────────────────────────────────────── */
#define UNIX_PATH_MAX       108

/* ── Ancillary data (cmsg) constants ─────────────────────────────── */
#define SOL_UNIX            1       /* SOL_SOCKET level for UNIX cmsg */

/* SCM (Socket Control Message) types for SOL_UNIX / SOL_SOCKET */
#define SCM_RIGHTS          1       /* Pass file descriptors */
#define SCM_CREDENTIALS     2       /* Pass process credentials */

/* ── Credentials structure (SCM_CREDENTIALS) ──────────────────────── */
struct ucred {
    uint32_t pid;           /* Process ID of sending process */
    uint32_t uid;           /* User ID of sending process */
    uint32_t gid;           /* Group ID of sending process */
};

/* ── Ancillary message header ─────────────────────────────────────── */
/*
 * struct cmsghdr — ancillary data header
 *
 * Used with sendmsg() and recvmsg() via struct msghdr::msg_control.
 * Each ancillary data object begins with a struct cmsghdr followed
 * by the cmsg_len bytes of payload (including the header).
 *
 * Alignment: cmsg_len and cmsg_data are aligned to sizeof(uint64_t).
 * The CMSG_ALIGN(), CMSG_DATA(), CMSG_NXTHDR(), and CMSG_FIRSTHDR()
 * macros provide portable access.
 */
struct cmsghdr {
    uint64_t cmsg_len;      /* Number of bytes including this header */
    int      cmsg_level;    /* Originating protocol (SOL_SOCKET, SOL_UNIX) */
    int      cmsg_type;     /* Protocol-specific type (SCM_RIGHTS, etc.) */
    /* Followed by unsigned char cmsg_data[]; access via CMSG_DATA() */
} __attribute__((packed));

/* Ancillary data alignment — must match tpaccet alignment (4 bytes
 * minimum; we use 8 bytes to align uint64_t members safely). */
#define CMSG_ALIGNMENT      sizeof(uint64_t)
#define CMSG_ALIGN(len)     (((len) + CMSG_ALIGNMENT - 1) & ~(CMSG_ALIGNMENT - 1))

/* Length of the cmsghdr structure including necessary trailing padding */
#define CMSG_LEN(len)       (CMSG_ALIGN(sizeof(struct cmsghdr)) + (len))

/* Total space occupied by a cmsg header + data, including final alignment */
#define CMSG_SPACE(len)     CMSG_ALIGN(CMSG_LEN(len))

/* Pointer to the data portion of a cmsghdr */
#define CMSG_DATA(cmsg)     ((unsigned char *)(cmsg) + CMSG_ALIGN(sizeof(struct cmsghdr)))

/* Pointer to the first ancillary data header in a msghdr */
#define CMSG_FIRSTHDR(msg)                                      \
    ((msg)->msg_control && (msg)->msg_controllen >= sizeof(struct cmsghdr) \
        ? (struct cmsghdr *)(msg)->msg_control : NULL)

/* Pointer to the next ancillary data header after the current one,
 * or NULL if there is none. */
#define CMSG_NXTHDR(msg, cmsg)                                          \
    (((unsigned char *)(cmsg) + CMSG_ALIGN((cmsg)->cmsg_len)            \
      >= (unsigned char *)(msg)->msg_control + (msg)->msg_controllen)   \
     ? NULL                                                              \
     : (struct cmsghdr *)((unsigned char *)(cmsg) + CMSG_ALIGN((cmsg)->cmsg_len)))

/* ── sockaddr_un ──────────────────────────────────────────────────── */
/* struct sockaddr_un is defined in socket.h:
 *
 * struct sockaddr_un {
 *     uint16_t sun_family;                // AF_UNIX (1)
 *     char     sun_path[UNIX_PATH_MAX];   // pathname (or abstract)
 * };
 *
 * Abstract socket namespace: if sun_path[0] == '\0', the name is in the
 * abstract namespace (not linked from the filesystem).  The remaining
 * sun_path bytes (up to UNIX_PATH_MAX - 1) form the abstract name.
 */

/* ── UNIX socket endpoint operations (internal kernel API) ────────── */
/* These are declared in socket.h and implemented in net/af_unix.c */

#endif /* AF_UNIX_H */

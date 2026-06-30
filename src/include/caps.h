#ifndef CAPS_H
#define CAPS_H

#include "types.h"

/* Forward declaration — process.h is included by calling code */
struct process;

/* System-wide capability bounding set management */

/* Initialize the global bounding set (all caps allowed by default) */
void sys_cap_bset_init(void);

/* Permanently drop a capability from the system-wide set */
void sys_cap_bset_drop(uint32_t cap);

/* Check if a capability is present in the system-wide set */
int  sys_cap_bset_has(uint32_t cap);

/* Apply the system-wide bounding set to a process's cap sets */
void sys_cap_bset_apply(struct process *proc);

/* POSIX capability numbers */
#define CAP_CHOWN           0
#define CAP_DAC_OVERRIDE    1
#define CAP_DAC_READ_SEARCH 2
#define CAP_FOWNER          3
#define CAP_FSETID          4
#define CAP_KILL            5
#define CAP_SETGID          6
#define CAP_SETUID          7
#define CAP_SETPCAP         8
#define CAP_NET_BIND_SERVICE 10
#define CAP_NET_BROADCAST   11
#define CAP_NET_ADMIN       12
#define CAP_NET_RAW         13
#define CAP_IPC_LOCK        14
#define CAP_IPC_OWNER       15
#define CAP_SYS_MODULE      16
#define CAP_SYS_RAWIO       17
#define CAP_SYS_CHROOT      18
#define CAP_SYS_PTRACE      19
#define CAP_SYS_PACCT       20
#define CAP_SYS_ADMIN       21
#define CAP_SYS_BOOT        22
#define CAP_SYS_NICE        23
#define CAP_SYS_RESOURCE    24
#define CAP_SYS_TIME        25
#define CAP_SYS_TTY_CONFIG  26
#define CAP_MKNOD           27
#define CAP_LEASE           28
#define CAP_AUDIT_WRITE     29
#define CAP_AUDIT_CONTROL   30
#define CAP_SETFCAP         31
#define CAP_MAC_OVERRIDE    32
#define CAP_MAC_ADMIN       33
#define CAP_SYSLOG          34
#define CAP_WAKE_ALARM      35
#define CAP_BLOCK_SUSPEND   36
#define CAP_AUDIT_READ      37
#define CAP_NET_ADMIN_RAW   38
#define CAP_LAST_CAP        38

#define CAP_BSET_SIZE       2  /* 64-bit words for up to 64 caps */

/* Linux __user_cap_header_struct/__user_cap_data_struct version constants */
#define _LINUX_CAPABILITY_VERSION_1  0x19980330
#define _LINUX_CAPABILITY_VERSION_2  0x20071026
#define _LINUX_CAPABILITY_VERSION_3  0x20080522
#define LINUX_CAPABILITY_VERSION     _LINUX_CAPABILITY_VERSION_3

/* User-space capability header (Linux ABI) */
struct __user_cap_header_struct {
    uint32_t version;
    int32_t  pid;
};

/* User-space capability data (Linux ABI, V1/V2/V3) */
struct __user_cap_data_struct {
    uint32_t effective;
    uint32_t permitted;
    uint32_t inheritable;
};

/* ── Capability audit enforcement ──────────────────────────────── */

/* Generic capable-with-audit check. Returns 0 if granted, -EPERM if denied. */
int cap_capable_audit(uint32_t cap, const char *audit_msg);

/* Convenience: CAP_SYS_RAWIO check for raw I/O operations */
int cap_sys_rawio_check(void);

/* Convenience: CAP_SYS_BOOT check for kexec_load */
int cap_sys_boot_check(void);

/* Convenience: CAP_SYS_MODULE check for init_module/finit_module */
int cap_sys_module_check(void);

#endif /* CAPS_H */

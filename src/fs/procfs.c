/*
 * procfs.c — /proc virtual filesystem
 *
 * Read-only VFS that exposes kernel state as pseudo-files.
 */

#include "vfs.h"
#include "timer.h"
#include "process.h"
#include "scheduler.h"
#include "pmm.h"
#include "heap.h"
#include "printf.h"
#include "string.h"
#include "types.h"
#include "net.h"

/* ─── Tiny snprintf-like helper ────────────────────────────────────────────── */

static void proc_u64_to_str(uint64_t v, char *buf, int *pos, int max) {
    if (v == 0) {
        if (*pos < max - 1) buf[(*pos)++] = '0';
        return;
    }
    char tmp[24]; int n = 0;
    while (v > 0) { tmp[n++] = '0' + (int)(v % 10); v /= 10; }
    while (n-- > 0 && *pos < max - 1) buf[(*pos)++] = tmp[n];
}

static void proc_str(const char *s, char *buf, int *pos, int max) {
    while (*s && *pos < max - 1) buf[(*pos)++] = *s++;
}

/* ─── File content generators ───────────────────────────────────────────────── */

static void proc_kb_line(const char *label, uint64_t bytes, char *buf, int *p, int max) {
    uint64_t kb = bytes / 1024;
    proc_str(label, buf, p, max);
    proc_u64_to_str(kb, buf, p, max);
    proc_str(" kB\n", buf, p, max);
}

static int procfs_gen_uptime(char *buf, int max) {
    int p = 0;
    uint64_t secs = timer_get_ticks() / TIMER_FREQ;
    proc_u64_to_str(secs, buf, &p, max);
    proc_str(" ", buf, &p, max);
    uint64_t idle = scheduler_get_idle_ticks() / TIMER_FREQ;
    proc_u64_to_str(idle, buf, &p, max);
    proc_str("\n", buf, &p, max);
    buf[p] = '\0';
    return p;
}

static int procfs_gen_meminfo(char *buf, int max) {
    int p = 0;
    uint64_t pmm_total = pmm_get_total_frames() * 4096;
    uint64_t pmm_free  = (pmm_get_total_frames() - pmm_get_used_frames()) * 4096;
    proc_kb_line("MemTotal:       ", pmm_total, buf, &p, max);
    proc_kb_line("MemFree:        ", pmm_free, buf, &p, max);
    proc_kb_line("HeapTotal:      ", heap_get_total(), buf, &p, max);
    proc_kb_line("HeapUsed:       ", heap_get_used(), buf, &p, max);
    proc_kb_line("HeapFree:       ", heap_get_free(), buf, &p, max);
    buf[p] = '\0';
    return p;
}

static int procfs_gen_cpuinfo(char *buf, int max) {
    int p = 0;
    uint32_t eax, ebx, ecx, edx;
    char vendor[13];

    /* Leaf 0: vendor string */
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));
    *(uint32_t*)&vendor[0] = ebx;
    *(uint32_t*)&vendor[4] = edx;
    *(uint32_t*)&vendor[8] = ecx;
    vendor[12] = 0;

    proc_str("processor\t: 0\n", buf, &p, max);
    proc_str("vendor_id\t: ", buf, &p, max);
    proc_str(vendor, buf, &p, max);
    proc_str("\n", buf, &p, max);

    /* Leaf 0x80000002-4: brand string */
    char brand[49];
    memset(brand, 0, 49);
    uint32_t max_ext;
    uint32_t _dummy_ebx, _dummy_ecx, _dummy_edx;
    __asm__ volatile("cpuid" : "=a"(max_ext), "=b"(_dummy_ebx), "=c"(_dummy_ecx), "=d"(_dummy_edx) : "a"(0x80000000));
    if (max_ext >= 0x80000004) {
        for (uint32_t i = 0; i < 3; i++) {
            __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000002 + i));
            *(uint32_t*)&brand[i*16+0] = eax;
            *(uint32_t*)&brand[i*16+4] = ebx;
            *(uint32_t*)&brand[i*16+8] = ecx;
            *(uint32_t*)&brand[i*16+12] = edx;
        }
    }
    if (brand[0]) {
        proc_str("model name\t: ", buf, &p, max);
        proc_str(brand, buf, &p, max);
        proc_str("\n", buf, &p, max);
    }

    /* Leaf 1: family/model/stepping + features */
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    proc_str("cpu family\t: ", buf, &p, max);
    proc_u64_to_str((eax >> 8) & 0xF, buf, &p, max);
    proc_str("\n", buf, &p, max);
    proc_str("model\t\t: ", buf, &p, max);
    proc_u64_to_str((eax >> 4) & 0xF, buf, &p, max);
    proc_str("\n", buf, &p, max);
    proc_str("stepping\t: ", buf, &p, max);
    proc_u64_to_str(eax & 0xF, buf, &p, max);
    proc_str("\n", buf, &p, max);

    /* Extended features from leaf 0x80000001 */
    if (max_ext >= 0x80000001) {
        uint32_t ex_eax, ex_ebx, ex_ecx, ex_edx;
        __asm__ volatile("cpuid" : "=a"(ex_eax), "=b"(ex_ebx), "=c"(ex_ecx), "=d"(ex_edx) : "a"(0x80000001));
        (void)ex_eax; (void)ex_ebx; (void)ex_ecx;
        if (ex_edx & (1u << 29)) {
            proc_str("flags\t\t: lm\n", buf, &p, max);
        }
    }

    buf[p] = '\0';
    return p;
}

static int procfs_gen_version(char *buf, int max) {
    int p = 0;
    proc_str("OS version 1.0 (x86_64)\n", buf, &p, max);
    buf[p] = '\0';
    return p;
}

static char *proc_arp_buf;
static int   proc_arp_pos;
static int   proc_arp_max;

static void proc_arp_cb(uint32_t ip, const uint8_t *mac) {
    proc_u64_to_str((ip >> 24) & 0xFF, proc_arp_buf, &proc_arp_pos, proc_arp_max);
    proc_str(".", proc_arp_buf, &proc_arp_pos, proc_arp_max);
    proc_u64_to_str((ip >> 16) & 0xFF, proc_arp_buf, &proc_arp_pos, proc_arp_max);
    proc_str(".", proc_arp_buf, &proc_arp_pos, proc_arp_max);
    proc_u64_to_str((ip >> 8) & 0xFF, proc_arp_buf, &proc_arp_pos, proc_arp_max);
    proc_str(".", proc_arp_buf, &proc_arp_pos, proc_arp_max);
    proc_u64_to_str(ip & 0xFF, proc_arp_buf, &proc_arp_pos, proc_arp_max);
    proc_str(" ", proc_arp_buf, &proc_arp_pos, proc_arp_max);
    for (int i = 0; i < 6; i++) {
        if (i) proc_str(":", proc_arp_buf, &proc_arp_pos, proc_arp_max);
        proc_u64_to_str(mac[i], proc_arp_buf, &proc_arp_pos, proc_arp_max);
    }
    proc_str("\n", proc_arp_buf, &proc_arp_pos, proc_arp_max);
}

static int procfs_gen_arp(char *buf, int max) {
    proc_arp_buf = buf;
    proc_arp_pos = 0;
    proc_arp_max = max;
    net_arp_list(proc_arp_cb);
    buf[proc_arp_pos] = '\0';
    return proc_arp_pos;
}

static int procfs_gen_route(char *buf, int max) {
    int p = 0;
    uint8_t ipb[4];
    net_get_ip(ipb);
    for (int i = 0; i < 4; i++) {
        if (i) proc_str(".", buf, &p, max);
        proc_u64_to_str(ipb[i], buf, &p, max);
    }
    proc_str(" dev eth0\n", buf, &p, max);
    uint32_t gw = net_get_gateway();
    if (gw) {
        proc_str("default via ", buf, &p, max);
        proc_u64_to_str((gw >> 24) & 0xFF, buf, &p, max);
        proc_str(".", buf, &p, max);
        proc_u64_to_str((gw >> 16) & 0xFF, buf, &p, max);
        proc_str(".", buf, &p, max);
        proc_u64_to_str((gw >> 8) & 0xFF, buf, &p, max);
        proc_str(".", buf, &p, max);
        proc_u64_to_str(gw & 0xFF, buf, &p, max);
        proc_str(" dev eth0\n", buf, &p, max);
    }
    buf[p] = '\0';
    return p;
}

static int procfs_gen_mounts(char *buf, int max) {
    int p = 0;
    char mnt[8][64];
    int n = vfs_list_mountpoints(mnt, 8);
    for (int i = 0; i < n; i++) {
        proc_str(mnt[i], buf, &p, max);
        proc_str(" / ", buf, &p, max);
        proc_str(mnt[i], buf, &p, max);
        proc_str("\n", buf, &p, max);
    }
    buf[p] = '\0';
    return p;
}

/* /proc/<pid>/status */
static int procfs_gen_pid_status(uint32_t pid, char *buf, int max) {
    struct process *p = process_get_by_pid(pid);
    if (!p || p->state == PROCESS_UNUSED) return -1;

    int pos = 0;
    proc_str("Name:\t", buf, &pos, max);
    proc_str(p->name ? p->name : "?", buf, &pos, max);
    proc_str("\n", buf, &pos, max);
    proc_str("Pid:\t", buf, &pos, max);
    proc_u64_to_str(p->pid, buf, &pos, max);
    proc_str("\n", buf, &pos, max);

    const char *state_str = "unknown";
    switch (p->state) {
        case PROCESS_READY:   state_str = "R (running)"; break;
        case PROCESS_RUNNING: state_str = "R (running)"; break;
        case PROCESS_BLOCKED: state_str = "S (sleeping)"; break;
        case PROCESS_ZOMBIE:  state_str = "Z (zombie)"; break;
        default: break;
    }
    proc_str("State:\t", buf, &pos, max);
    proc_str(state_str, buf, &pos, max);
    proc_str("\n", buf, &pos, max);
    buf[pos] = '\0';
    return pos;
}

/* ─── VFS ops ────────────────────────────────────────────────────────────────── */

static int procfs_read(void *priv, const char *path, void *buf_v,
                       uint32_t max_size, uint32_t *out_size) {
    (void)priv;
    char *buf = (char *)buf_v;
    int len = 0;

    if (strcmp(path, "/proc/uptime") == 0) {
        len = procfs_gen_uptime(buf, (int)max_size);
    } else if (strcmp(path, "/proc/meminfo") == 0) {
        len = procfs_gen_meminfo(buf, (int)max_size);
    } else if (strcmp(path, "/proc/cpuinfo") == 0) {
        len = procfs_gen_cpuinfo(buf, (int)max_size);
    } else if (strcmp(path, "/proc/version") == 0) {
        len = procfs_gen_version(buf, (int)max_size);
    } else if (strcmp(path, "/proc/net/arp") == 0) {
        len = procfs_gen_arp(buf, (int)max_size);
    } else if (strcmp(path, "/proc/net/route") == 0) {
        len = procfs_gen_route(buf, (int)max_size);
    } else if (strcmp(path, "/proc/mounts") == 0) {
        len = procfs_gen_mounts(buf, (int)max_size);
    } else {
        /* Try /proc/<pid>/status */
        const char *p = path + 6; /* skip "/proc/" */
        uint32_t pid = 0; int got = 0;
        while (*p >= '0' && *p <= '9') { pid = pid * 10 + (uint32_t)(*p - '0'); p++; got = 1; }
        if (got && strcmp(p, "/status") == 0) {
            len = procfs_gen_pid_status(pid, buf, (int)max_size);
            if (len < 0) return -1;
        } else {
            return -1;
        }
    }

    if (out_size) *out_size = (uint32_t)len;
    return 0;
}

static int procfs_stat(void *priv, const char *path, struct vfs_stat *st) {
    (void)priv;
    /* /proc itself is a directory */
    if (strcmp(path, "/proc") == 0) {
        st->type = 2; st->size = 0; return 0;
    }
    /* Known files */
    if (strcmp(path, "/proc/uptime") == 0 ||
        strcmp(path, "/proc/meminfo") == 0 ||
        strcmp(path, "/proc/cpuinfo") == 0 ||
        strcmp(path, "/proc/version") == 0 ||
        strcmp(path, "/proc/net/arp") == 0 ||
        strcmp(path, "/proc/net/route") == 0 ||
        strcmp(path, "/proc/mounts") == 0) {
        st->type = 1; st->size = 128; return 0;
    }
    /* /proc/<pid>/status */
    const char *p = path + 6;
    uint32_t pid = 0; int got = 0;
    while (*p >= '0' && *p <= '9') { pid = pid * 10 + (uint32_t)(*p - '0'); p++; got = 1; }
    if (got && strcmp(p, "/status") == 0) {
        struct process *proc = process_get_by_pid(pid);
        if (proc && proc->state != PROCESS_UNUSED) {
            st->type = 1; st->size = 256; return 0;
        }
    }
    return -1;
}

static int procfs_readdir(void *priv, const char *path) {
    (void)priv;
    if (strcmp(path, "/proc") != 0) return -1;
    kprintf("uptime\nmeminfo\ncpuinfo\nversion\nnet\nmounts\n");
    /* Also list active PIDs */
    struct process *table = process_get_table();
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (table[i].state != PROCESS_UNUSED) {
            kprintf("%u\n", (uint64_t)table[i].pid);
        }
    }
    return 0;
}

struct vfs_ops procfs_ops = {
    .read    = procfs_read,
    .write   = NULL,
    .stat    = procfs_stat,
    .create  = NULL,
    .unlink  = NULL,
    .readdir = procfs_readdir,
};

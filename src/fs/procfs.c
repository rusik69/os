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
#include "smp.h"

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

    /* Number of CPUs online */
    int cpu_count = smp_get_cpu_count();
    if (cpu_count < 1) cpu_count = 1;

    for (int cpu = 0; cpu < cpu_count; cpu++) {
        proc_str("processor\t: ", buf, &p, max);
        proc_u64_to_str((uint64_t)cpu, buf, &p, max);
        proc_str("\n", buf, &p, max);

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

        /* Features from CPUID leaves */
        proc_str("flags\t\t: ", buf, &p, max);
        if (edx & (1 << 0))  proc_str("fpu ", buf, &p, max);
        if (edx & (1 << 1))  proc_str("vme ", buf, &p, max);
        if (edx & (1 << 2))  proc_str("de ", buf, &p, max);
        if (edx & (1 << 3))  proc_str("pse ", buf, &p, max);
        if (edx & (1 << 4))  proc_str("tsc ", buf, &p, max);
        if (edx & (1 << 5))  proc_str("msr ", buf, &p, max);
        if (edx & (1 << 6))  proc_str("pae ", buf, &p, max);
        if (edx & (1 << 7))  proc_str("mce ", buf, &p, max);
        if (edx & (1 << 8))  proc_str("cx8 ", buf, &p, max);
        if (edx & (1 << 9))  proc_str("apic ", buf, &p, max);
        if (edx & (1 << 11)) proc_str("sep ", buf, &p, max);
        if (edx & (1 << 12)) proc_str("mtrr ", buf, &p, max);
        if (edx & (1 << 13)) proc_str("pge ", buf, &p, max);
        if (edx & (1 << 14)) proc_str("mca ", buf, &p, max);
        if (edx & (1 << 15)) proc_str("cmov ", buf, &p, max);
        if (edx & (1 << 16)) proc_str("pat ", buf, &p, max);
        if (edx & (1 << 17)) proc_str("pse36 ", buf, &p, max);
        if (edx & (1 << 18)) proc_str("pn ", buf, &p, max);
        if (edx & (1 << 19)) proc_str("clflush ", buf, &p, max);
        if (edx & (1 << 23)) proc_str("mmx ", buf, &p, max);
        if (edx & (1 << 24)) proc_str("fxsr ", buf, &p, max);
        if (edx & (1 << 25)) proc_str("sse ", buf, &p, max);
        if (edx & (1 << 26)) proc_str("sse2 ", buf, &p, max);
        if (ecx & (1 << 0))  proc_str("sse3 ", buf, &p, max);
        if (ecx & (1 << 9))  proc_str("ssse3 ", buf, &p, max);
        if (ecx & (1 << 19)) proc_str("sse4.1 ", buf, &p, max);
        if (ecx & (1 << 20)) proc_str("sse4.2 ", buf, &p, max);
        if (ecx & (1 << 29)) proc_str("f16c ", buf, &p, max);
        if (edx & (1 << 28)) proc_str("ht ", buf, &p, max);
        /* Extended features from leaf 0x80000001 */
        if (max_ext >= 0x80000001) {
            uint32_t ex_eax, ex_ebx, ex_ecx, ex_edx;
            __asm__ volatile("cpuid" : "=a"(ex_eax), "=b"(ex_ebx), "=c"(ex_ecx), "=d"(ex_edx) : "a"(0x80000001));
            (void)ex_eax; (void)ex_ebx; (void)ex_ecx;
            if (ex_edx & (1u << 29)) proc_str("lm ", buf, &p, max);
            if (ex_edx & (1u << 26)) proc_str("nx ", buf, &p, max);
            if (ex_edx & (1u << 11)) proc_str("syscall ", buf, &p, max);
            if (ex_edx & (1u << 27)) proc_str("rdtscp ", buf, &p, max);
        }
        proc_str("\n", buf, &p, max);

        /* Caches from leaf 2 or 4 could be added here */
        /* CPU MHz from leaf 0x16 (if available) or TSC calibration */
        proc_str("cpu MHz\t\t: 0.000\n", buf, &p, max);
        proc_str("\n", buf, &p, max);
    }

    buf[p] = '\0';
    return p;
}

static int procfs_gen_version(char *buf, int max) {
    int p = 0;
    proc_str("OS Kernel v1.0 (gcc 13.3.0, x86_64)\n", buf, &p, max);
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
    proc_str("PPid:\t", buf, &pos, max);
    proc_u64_to_str(p->parent_pid, buf, &pos, max);
    proc_str("\n", buf, &pos, max);
    proc_str("Tgid:\t", buf, &pos, max);
    proc_u64_to_str(p->tgid, buf, &pos, max);
    proc_str("\n", buf, &pos, max);
    proc_str("Priority:\t", buf, &pos, max);
    proc_u64_to_str(p->priority, buf, &pos, max);
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
    proc_str("Utime:\t", buf, &pos, max);
    proc_u64_to_str(p->utime_ticks, buf, &pos, max);
    proc_str("\n", buf, &pos, max);
    proc_str("Stime:\t", buf, &pos, max);
    proc_u64_to_str(p->stime_ticks, buf, &pos, max);
    proc_str("\n", buf, &pos, max);
    proc_str("nvcsw:\t", buf, &pos, max);
    proc_u64_to_str(p->nvcsw, buf, &pos, max);
    proc_str("\n", buf, &pos, max);
    proc_str("nivcsw:\t", buf, &pos, max);
    proc_u64_to_str(p->nivcsw, buf, &pos, max);
    proc_str("\n", buf, &pos, max);
    proc_str("minflt:\t", buf, &pos, max);
    proc_u64_to_str(p->minflt, buf, &pos, max);
    proc_str("\n", buf, &pos, max);
    proc_str("majflt:\t", buf, &pos, max);
    proc_u64_to_str(p->majflt, buf, &pos, max);
    buf[pos] = '\0';
    return pos;
}

/* /proc/stat — CPU and system statistics */
static int procfs_gen_stat(char *buf, int max) {
    int p = 0;
    uint64_t now = timer_get_ticks();
    uint64_t idle_ticks = scheduler_get_idle_ticks();

    /* Aggregate per-process CPU time into system-wide counters */
    uint64_t user_ticks = 0, system_ticks = 0;
    struct process *table = process_get_table();
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (table[i].state == PROCESS_UNUSED) continue;
        user_ticks   += table[i].utime_ticks;
        system_ticks += table[i].stime_ticks;
    }

    /* Linux /proc/stat format: cpu user nice system idle iowait irq softirq steal */
    proc_str("cpu  ", buf, &p, max);
    proc_u64_to_str(user_ticks, buf, &p, max);      /* user */
    proc_str(" 0 ", buf, &p, max);                   /* nice */
    proc_u64_to_str(system_ticks, buf, &p, max);    /* system */
    proc_str(" ", buf, &p, max);
    proc_u64_to_str(idle_ticks, buf, &p, max);      /* idle */
    proc_str(" 0 0 0 0\n", buf, &p, max);            /* iowait irq softirq steal */

    proc_str("procs_running ", buf, &p, max);
    int running = 0;
    for (int i = 0; i < PROCESS_MAX; i++)
        if (table[i].state == PROCESS_RUNNING || table[i].state == PROCESS_READY)
            running++;
    proc_u64_to_str(running, buf, &p, max);
    proc_str("\n", buf, &p, max);

    proc_str("procs_blocked ", buf, &p, max);
    int blocked = 0;
    for (int i = 0; i < PROCESS_MAX; i++)
        if (table[i].state == PROCESS_BLOCKED)
            blocked++;
    proc_u64_to_str(blocked, buf, &p, max);
    proc_str("\n", buf, &p, max);

    proc_str("btime ", buf, &p, max);
    proc_u64_to_str(0, buf, &p, max); /* boot time (0 = not tracked) */
    proc_str("\n", buf, &p, max);

    buf[p] = '\0';
    return p;
}

/* /proc/loadavg */
static int procfs_gen_loadavg(char *buf, int max) {
    int p = 0;
    /* Simple: count running/ready processes as load */
    int run = 0, total = 0;
    struct process *table = process_get_table();
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (table[i].state != PROCESS_UNUSED) {
            total++;
            if (table[i].state == PROCESS_RUNNING || table[i].state == PROCESS_READY)
                run++;
        }
    }

    /* Show runnable/total as load averages */
    proc_u64_to_str(run, buf, &p, max);
    proc_str(".00 ", buf, &p, max);
    proc_u64_to_str(run, buf, &p, max);
    proc_str(".00 ", buf, &p, max);
    proc_u64_to_str(run, buf, &p, max);
    proc_str(".00 ", buf, &p, max);
    proc_u64_to_str(run, buf, &p, max);
    proc_str("/", buf, &p, max);
    proc_u64_to_str(total, buf, &p, max);
    proc_str(" ", buf, &p, max);
    proc_u64_to_str(run, buf, &p, max);
    proc_str("\n", buf, &p, max);

    buf[p] = '\0';
    return p;
}

/* /proc/<pid>/fd — list open file descriptors */
static int procfs_gen_pid_fd(uint32_t pid, char *buf, int max) {
    struct process *p = process_get_by_pid(pid);
    if (!p || p->state == PROCESS_UNUSED) return -1;

    int pos = 0;
    for (int i = 0; i < PROCESS_FD_MAX; i++) {
        if (!p->fd_table[i].used) continue;
        proc_u64_to_str((uint64_t)i, buf, &pos, max);
        proc_str(" -> ", buf, &pos, max);
        proc_str(p->fd_table[i].path, buf, &pos, max);
        proc_str("\n", buf, &pos, max);
    }
    buf[pos] = '\0';
    return pos;
}

/* /proc/<pid>/cmdline — command line (just the name for now) */
static int procfs_gen_pid_cmdline(uint32_t pid, char *buf, int max) {
    struct process *p = process_get_by_pid(pid);
    if (!p || p->state == PROCESS_UNUSED) return -1;

    int pos = 0;
    if (p->name) {
        proc_str(p->name, buf, &pos, max);
    }
    buf[pos] = '\0';
    return pos;
}

/* ─── VFS ops ────────────────────────────────────────────────────────────────── */

static int procfs_read(void *priv, const char *path, void *buf_v,
                       uint32_t max_size, uint32_t *out_size) {
    (void)priv;
    char *buf = (char *)buf_v;
    int len = 0;

    /* Resolve /proc/self dynamically to /proc/<current_pid> */
    if (strncmp(path, "/proc/self", 10) == 0) {
        struct process *proc = process_get_current();
        if (!proc) return -1;
        /* Build a temporary resolved path for the sub-path */
        char resolved[128];
        int pos = 0;
        const char *prefix = "/proc/";
        while (*prefix) resolved[pos++] = *prefix++;
        /* Append PID digits */
        uint64_t pid = proc->pid;
        char pid_str[12];
        int pi = 0;
        if (pid == 0) { pid_str[pi++] = '0'; }
        else { char tmp[12]; int ti = 0; while (pid) { tmp[ti++] = '0' + (int)(pid % 10); pid /= 10; } while (ti > 0) pid_str[pi++] = tmp[--ti]; }
        for (int i = 0; i < pi; i++) resolved[pos++] = pid_str[i];
        /* Append the rest of the path after /proc/self */
        const char *rest = path + 10;
        while (*rest && pos < 126) resolved[pos++] = *rest++;
        resolved[pos] = '\0';
        /* Recursively read using the resolved path */
        return procfs_read(priv, resolved, buf_v, max_size, out_size);
    }

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
    } else if (strcmp(path, "/proc/stat") == 0) {
        len = procfs_gen_stat(buf, (int)max_size);
    } else if (strcmp(path, "/proc/loadavg") == 0) {
        len = procfs_gen_loadavg(buf, (int)max_size);
    } else if (strcmp(path, "/proc/self") == 0) {
        /* Redirect to status of current process */
        struct process *proc = process_get_current();
        if (proc)
            len = procfs_gen_pid_status(proc->pid, buf, (int)max_size);
        else
            return -1;
    } else {
        /* Try /proc/<pid>/status */
        const char *p = path + 6; /* skip "/proc/" */
        uint32_t pid = 0; int got = 0;
        while (*p >= '0' && *p <= '9') { pid = pid * 10 + (uint32_t)(*p - '0'); p++; got = 1; }
        if (got && strcmp(p, "/status") == 0) {
            len = procfs_gen_pid_status(pid, buf, (int)max_size);
            if (len < 0) return -1;
        } else if (got && strcmp(p, "/fd") == 0) {
            len = procfs_gen_pid_fd(pid, buf, (int)max_size);
            if (len < 0) return -1;
        } else if (got && strcmp(p, "/cmdline") == 0) {
            len = procfs_gen_pid_cmdline(pid, buf, (int)max_size);
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
        strcmp(path, "/proc/mounts") == 0 ||
        strcmp(path, "/proc/stat") == 0 ||
        strcmp(path, "/proc/loadavg") == 0) {
        st->type = 1; st->size = 256; return 0;
    }
    /* /proc/self is a symlink to /proc/<pid>/ — return directory type */
    if (strcmp(path, "/proc/self") == 0) {
        st->type = 2; st->size = 0; return 0;
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
    if (got && strcmp(p, "/fd") == 0) {
        struct process *proc = process_get_by_pid(pid);
        if (proc && proc->state != PROCESS_UNUSED) {
            st->type = 1; st->size = 512; return 0;
        }
    }
    if (got && strcmp(p, "/cmdline") == 0) {
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
    kprintf("uptime\nmeminfo\ncpuinfo\nversion\nself\nstat\nloadavg\nnet\nmounts\n");
    /* Also list active PIDs */
    struct process *table = process_get_table();
    struct process *caller = process_get_current();
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (table[i].state != PROCESS_UNUSED) {
            if (!caller || process_can_see(caller, &table[i]))
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

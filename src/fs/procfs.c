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
#include "net_internal.h"
#include "smp.h"
#include "vmm.h"
#include "slab.h"
#include "sysctl.h"
#include "config_gz.h"
#include "process_rlimit.h"
#include "idt.h"
#include "psi.h"
#include "cgroup_namespace.h"
#include "cpu_topology.h"
#include "sysrq.h"
#include "module.h"

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

/* ── /proc/interrupts: per-CPU per-vector interrupt counts ────────── */

static int procfs_gen_interrupts(char *buf, int max) {
    int p = 0;
    int cpu_count = smp_get_cpu_count();
    if (cpu_count < 1) cpu_count = 1;
    int max_cpus = (cpu_count > IDT_NR_CPUS) ? IDT_NR_CPUS : cpu_count;

    /* Header: right-aligned CPU columns */
    proc_str("           ", buf, &p, max);
    for (int cpu = 0; cpu < max_cpus; cpu++) {
        proc_str(" CPU", buf, &p, max);
        int cpu_d = cpu;
        /* Format CPU number right-aligned in 6 chars */
        char fmt[8]; int fn = 0;
        if (cpu_d == 0) { fmt[fn++] = '0'; }
        else { char tmp[8]; int tn = 0; while (cpu_d) { tmp[tn++] = '0' + cpu_d % 10; cpu_d /= 10; } while (tn > 0) fmt[fn++] = tmp[--tn]; }
        fmt[fn] = '\0';
        /* Pad to 6 chars */
        int pad = 6 - fn;
        while (pad-- > 0 && p < max - 1) buf[p++] = ' ';
        for (int i = 0; i < fn && p < max - 1; i++) buf[p++] = fmt[i];
    }
    proc_str("       \n", buf, &p, max);

    /* Walk vectors 0..255, show those with a name or non-zero count */
    for (int vec = 0; vec < IDT_NUM_VECTORS; vec++) {
        const char *vname = idt_get_vector_name(vec);
        /* Skip vectors that have no name AND zero count on all CPUs */
        int all_zero = 1;
        for (int cpu = 0; cpu < max_cpus; cpu++) {
            if (idt_get_irq_count(cpu, vec) != 0) { all_zero = 0; break; }
        }
        if (!vname && all_zero) continue;

        /* Vector number: right-aligned in 4 chars */
        char vstr[8]; int vn = 0;
        if (vec == 0) { vstr[vn++] = '0'; }
        else { char tmp[8]; int tn = 0; int v = vec; while (v) { tmp[tn++] = '0' + v % 10; v /= 10; } while (tn > 0) vstr[vn++] = tmp[--tn]; }
        vstr[vn] = '\0';
        int pad_v = 4 - vn;
        while (pad_v-- > 0 && p < max - 1) buf[p++] = ' ';
        for (int i = 0; i < vn && p < max - 1; i++) buf[p++] = vstr[i];
        proc_str(": ", buf, &p, max);

        /* Per-CPU counts right-aligned in 8 chars */
        for (int cpu = 0; cpu < max_cpus; cpu++) {
            uint64_t cnt = idt_get_irq_count(cpu, vec);
            char cstr[24]; int cn = 0;
            if (cnt == 0) { cstr[cn++] = '0'; }
            else { char tmp[24]; int tn = 0; while (cnt) { tmp[tn++] = '0' + (int)(cnt % 10); cnt /= 10; } while (tn > 0) cstr[cn++] = tmp[--tn]; }
            cstr[cn] = '\0';
            int pad_c = 8 - cn;
            while (pad_c-- > 0 && p < max - 1) buf[p++] = ' ';
            for (int i = 0; i < cn && p < max - 1; i++) buf[p++] = cstr[i];
        }

        /* Controller type + name */
        if (vec >= 32 && vec < 48) {
            proc_str("  IO-APIC", buf, &p, max);
        } else if (vec >= 240 && vec <= 243) {
            proc_str("  IPI", buf, &p, max);
        } else if (vec < 32) {
            proc_str("  CPU-exc", buf, &p, max);
        } else {
            proc_str("  generic", buf, &p, max);
        }
        if (vname) {
            proc_str("    ", buf, &p, max);
            proc_str(vname, buf, &p, max);
        }
        proc_str("\n", buf, &p, max);

        if (p >= max - 2) break; /* prevent buffer overflow */
    }

    buf[p] = '\0';
    return p;
}

static int procfs_gen_meminfo(char *buf, int max) {
    int p = 0;
    uint64_t pmm_total = pmm_get_total_frames() * 4096;
    uint64_t pmm_free  = (pmm_get_total_frames() - pmm_get_used_frames()) * 4096;
    proc_kb_line("MemTotal:       ", pmm_total, buf, &p, max);
    proc_kb_line("MemFree:        ", pmm_free, buf, &p, max);
    proc_kb_line("Buffers:        ", 0, buf, &p, max);
    proc_kb_line("Cached:         ", 0, buf, &p, max);
    proc_kb_line("SwapTotal:      ", 0, buf, &p, max);
    proc_kb_line("SwapFree:       ", 0, buf, &p, max);
    proc_kb_line("Dirty:          ", 0, buf, &p, max);
    proc_kb_line("Writeback:      ", 0, buf, &p, max);
    proc_kb_line("Mapped:         ", 0, buf, &p, max);
    proc_kb_line("PageTables:     ", 0, buf, &p, max);
    proc_kb_line("VmallocTotal:   ", 0, buf, &p, max);
    proc_kb_line("VmallocUsed:    ", 0, buf, &p, max);
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

/* /proc/net/dev — interface statistics (Linux: Inter-|   Receive  |  Transmit) */
static int procfs_gen_net_dev(char *buf, int max) {
    int p = 0;
    proc_str("Inter-|   Receive                                                |  Transmit\n", buf, &p, max);
    proc_str(" face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n", buf, &p, max);
    proc_str("  eth0: ", buf, &p, max);
    proc_u64_to_str(net_iface_stats.rx_bytes, buf, &p, max);
    proc_str(" ", buf, &p, max);
    proc_u64_to_str(net_iface_stats.rx_packets, buf, &p, max);
    proc_str(" ", buf, &p, max);
    proc_u64_to_str(net_iface_stats.rx_errors, buf, &p, max);
    proc_str(" ", buf, &p, max);
    proc_u64_to_str(net_iface_stats.rx_drops, buf, &p, max);
    proc_str(" 0 0 0 0 ", buf, &p, max);
    proc_u64_to_str(net_iface_stats.tx_bytes, buf, &p, max);
    proc_str(" ", buf, &p, max);
    proc_u64_to_str(net_iface_stats.tx_packets, buf, &p, max);
    proc_str(" ", buf, &p, max);
    proc_u64_to_str(net_iface_stats.tx_errors, buf, &p, max);
    proc_str(" ", buf, &p, max);
    proc_u64_to_str(net_iface_stats.tx_drops, buf, &p, max);
    proc_str(" 0 0 0 0\n", buf, &p, max);
    /* Also show loopback */
    proc_str("    lo: 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n", buf, &p, max);
    buf[p] = '\0';
    return p;
}

/* /proc/net/tcp — TCP connection table */
static char *proc_tcp_buf;
static int   proc_tcp_pos;
static int   proc_tcp_max;

static void proc_tcp_entry_cb(uint16_t lport, uint32_t rip, uint16_t rport, int state) {
    if (proc_tcp_pos >= proc_tcp_max - 1) return;
    /* Format: sl local_address rem_address st tx_queue rx_queue ... */
    /* Convert IP to hex manually using nibble helper */
    uint8_t ip_bytes[4];
    net_get_ip(ip_bytes);

    /* Write local_addr:port as hex */
    for (int i = 0; i < 4; i++) {
        uint8_t hi = (ip_bytes[i] >> 4) & 0xF;
        uint8_t lo = ip_bytes[i] & 0xF;
        proc_tcp_buf[proc_tcp_pos++] = hi < 10 ? '0' + hi : 'a' + hi - 10;
        proc_tcp_buf[proc_tcp_pos++] = lo < 10 ? '0' + lo : 'a' + lo - 10;
        if (proc_tcp_pos >= proc_tcp_max - 1) return;
    }
    proc_tcp_buf[proc_tcp_pos++] = ':';
    if (proc_tcp_pos >= proc_tcp_max - 1) return;
    uint8_t ph = (lport >> 8) & 0xFF;
    uint8_t pl = lport & 0xFF;
    proc_tcp_buf[proc_tcp_pos++] = ph < 10 ? '0' + ph : 'a' + ph - 10;
    if (proc_tcp_pos >= proc_tcp_max - 1) return;
    proc_tcp_buf[proc_tcp_pos++] = pl < 10 ? '0' + pl : 'a' + pl - 10;
    if (proc_tcp_pos >= proc_tcp_max - 1) return;
    proc_tcp_buf[proc_tcp_pos++] = ' ';
    if (proc_tcp_pos >= proc_tcp_max - 1) return;

    /* Remote addr:port */
    uint8_t rip_bytes[4];
    rip_bytes[0] = (rip >> 24) & 0xFF;
    rip_bytes[1] = (rip >> 16) & 0xFF;
    rip_bytes[2] = (rip >> 8) & 0xFF;
    rip_bytes[3] = rip & 0xFF;
    for (int i = 0; i < 4; i++) {
        uint8_t hi = (rip_bytes[i] >> 4) & 0xF;
        uint8_t lo = rip_bytes[i] & 0xF;
        proc_tcp_buf[proc_tcp_pos++] = hi < 10 ? '0' + hi : 'a' + hi - 10;
        proc_tcp_buf[proc_tcp_pos++] = lo < 10 ? '0' + lo : 'a' + lo - 10;
        if (proc_tcp_pos >= proc_tcp_max - 1) return;
    }
    proc_tcp_buf[proc_tcp_pos++] = ':';
    if (proc_tcp_pos >= proc_tcp_max - 1) return;
    ph = (rport >> 8) & 0xFF;
    pl = rport & 0xFF;
    proc_tcp_buf[proc_tcp_pos++] = ph < 10 ? '0' + ph : 'a' + ph - 10;
    if (proc_tcp_pos >= proc_tcp_max - 1) return;
    proc_tcp_buf[proc_tcp_pos++] = pl < 10 ? '0' + pl : 'a' + pl - 10;
    if (proc_tcp_pos >= proc_tcp_max - 1) return;
    proc_tcp_buf[proc_tcp_pos++] = ' ';
    if (proc_tcp_pos >= proc_tcp_max - 1) return;

    /* State in hex */
    uint8_t sh = (state >> 4) & 0xF;
    uint8_t sl2 = state & 0xF;
    proc_tcp_buf[proc_tcp_pos++] = sh < 10 ? '0' + sh : 'a' + sh - 10;
    if (proc_tcp_pos >= proc_tcp_max - 1) return;
    proc_tcp_buf[proc_tcp_pos++] = sl2 < 10 ? '0' + sl2 : 'a' + sl2 - 10;
    if (proc_tcp_pos >= proc_tcp_max - 1) return;
    proc_tcp_buf[proc_tcp_pos++] = ' ';
    if (proc_tcp_pos >= proc_tcp_max - 1) return;

    /* tx_queue:rx_queue (both 0 for now) */
    proc_tcp_buf[proc_tcp_pos++] = '0'; proc_tcp_buf[proc_tcp_pos++] = ':'; proc_tcp_buf[proc_tcp_pos++] = '0';
    if (proc_tcp_pos >= proc_tcp_max - 1) return;
    proc_str(" 00:00000000 00000000     0 1 0\n", proc_tcp_buf, &proc_tcp_pos, proc_tcp_max);
}

static int procfs_gen_net_tcp(char *buf, int max) {
    int p = 0;
    proc_str("  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n", buf, &p, max);
    buf[p] = '\0';
    proc_tcp_buf = buf + p;
    proc_tcp_pos = p;
    proc_tcp_max = max;
    net_conn_list(proc_tcp_entry_cb);
    if (proc_tcp_pos < max) buf[proc_tcp_pos] = '\0';
    return proc_tcp_pos < max ? proc_tcp_pos : max - 1;
}

static int procfs_gen_mounts(char *buf, int max) {
    int p = 0;
    char mnt[8][64];
    int n = vfs_list_mountpoints(mnt, 8);
    for (int i = 0; i < n; i++) {
        /* Format: device mount_point fstype flags */
        proc_str("none ", buf, &p, max);
        proc_str(mnt[i], buf, &p, max);
        proc_str(" ", buf, &p, max);
        if (strcmp(mnt[i], "/proc") == 0)
            proc_str("proc ", buf, &p, max);
        else if (strcmp(mnt[i], "/dev") == 0)
            proc_str("devfs ", buf, &p, max);
        else if (strcmp(mnt[i], "/mnt") == 0)
            proc_str("fat32 ", buf, &p, max);
        else
            proc_str("smfs ", buf, &p, max);
        proc_str("rw 0 0\n", buf, &p, max);
    }
    buf[p] = '\0';
    return p;
}

/* /proc/filesystems — registered filesystem types */
static int procfs_gen_filesystems(char *buf, int max) {
    int p = 0;
    char names[VFS_MAX_FS_TYPES][32];
    int n = vfs_list_filesystems(names, VFS_MAX_FS_TYPES);
    proc_str("nodev\tsysfs\n", buf, &p, max);
    proc_str("nodev\tproc\n", buf, &p, max);
    proc_str("nodev\tdevfs\n", buf, &p, max);
    proc_str("nodev\ttmpfs\n", buf, &p, max);
    proc_str("\tsmfs\n", buf, &p, max);
    proc_str("\tfat32\n", buf, &p, max);
    for (int i = 0; i < n; i++) {
        proc_str("\t", buf, &p, max);
        proc_str(names[i], buf, &p, max);
        proc_str("\n", buf, &p, max);
    }
    buf[p] = '\0';
    return p;
}

/* /proc/modules — loaded kernel modules (Linux-compatible format) */
static int procfs_gen_modules(char *buf, int max) {
    int p = 0;

    /* Linux /proc/modules format:
     *   name size used_by [state]
     * e.g.:
     *   e1000 16384 - Live
     *   ext2 53248 - Live
     *   fat32 24576 vfat Live
     */
    for (int i = 0; i < MODULE_MAX && p < max - 128; i++) {
        struct kernel_module *mod = module_get_by_id(i);
        if (!mod)
            continue;

        /* Module name */
        proc_str(mod->name, buf, &p, max);
        proc_str(" ", buf, &p, max);

        /* Size — approximate from section sizes, fallback to total size */
        uint64_t total_size = mod->size;
        if (total_size == 0) {
            for (int s = 0; s < mod->num_sections && s < 16; s++) {
                if (mod->sections[s].size > 0)
                    total_size += mod->sections[s].size;
            }
        }
        /* Print size in bytes (Linux uses decimal) */
        char size_str[24];
        int sl = sprintf(size_str, "%llu", (unsigned long long)total_size);
        if (sl > 0 && sl < (int)sizeof(size_str))
            proc_str(size_str, buf, &p, max);

        /* Used-by list — iterate dependencies */
        proc_str(" ", buf, &p, max);
        int dep_count = 0;
        for (int d = 0; d < mod->num_deps && d < MODULE_MAX_DEPS; d++) {
            if (mod->deps[d].name[0] && mod->deps[d].loaded) {
                if (dep_count > 0)
                    proc_str(",", buf, &p, max);
                proc_str(mod->deps[d].name, buf, &p, max);
                dep_count++;
            }
        }
        if (dep_count == 0)
            proc_str("-", buf, &p, max);

        /* State */
        proc_str(" Live", buf, &p, max);

        /* Module refcount (Linux shows this on newer kernels) */
        char ref_str[16];
        int rl = sprintf(ref_str, " %d", mod->refcount);
        if (rl > 0 && rl < (int)sizeof(ref_str))
            proc_str(ref_str, buf, &p, max);

        proc_str("\n", buf, &p, max);
    }

    buf[p] = '\0';
    return p;
}

/* /proc/vmstat — virtual memory statistics */
static int procfs_gen_vmstat(char *buf, int max) {
    int p = 0;
    proc_str("pgalloc ", buf, &p, max); proc_u64_to_str(vm_pgalloc, buf, &p, max); proc_str("\n", buf, &p, max);
    proc_str("pgfree ", buf, &p, max); proc_u64_to_str(vm_pgfree, buf, &p, max); proc_str("\n", buf, &p, max);
    proc_str("pgfault ", buf, &p, max); proc_u64_to_str(vm_pgfault, buf, &p, max); proc_str("\n", buf, &p, max);
    proc_str("pgmajfault ", buf, &p, max); proc_u64_to_str(vm_pgmajfault, buf, &p, max); proc_str("\n", buf, &p, max);
    proc_str("pgswapin ", buf, &p, max); proc_u64_to_str(vm_pgswapin, buf, &p, max); proc_str("\n", buf, &p, max);
    proc_str("pgswapout ", buf, &p, max); proc_u64_to_str(vm_pgswapout, buf, &p, max); proc_str("\n", buf, &p, max);
    proc_str("pgin ", buf, &p, max); proc_u64_to_str(vm_pgin, buf, &p, max); proc_str("\n", buf, &p, max);
    proc_str("pgout ", buf, &p, max); proc_u64_to_str(vm_pgout, buf, &p, max); proc_str("\n", buf, &p, max);
    buf[p] = '\0';
    return p;
}

/* /proc/slabinfo — slab allocator statistics */
static int procfs_gen_slabinfo(char *buf, int max) {
    int p = 0;
    struct slab_stats s;
    slab_get_stats(&s);
    proc_str("slabinfo - version: 1.0\n", buf, &p, max);
    proc_str("# name            <total_objs> <used_objs> <cache_count> <memory_used>\n", buf, &p, max);
    proc_u64_to_str(s.total_objects, buf, &p, max);
    proc_str(" ", buf, &p, max);
    proc_u64_to_str(s.used_objects, buf, &p, max);
    proc_str(" ", buf, &p, max);
    proc_u64_to_str(s.cache_count, buf, &p, max);
    proc_str(" ", buf, &p, max);
    proc_u64_to_str(s.memory_used, buf, &p, max);
    proc_str("\n", buf, &p, max);
    buf[p] = '\0';
    return p;
}

/* /proc/<pid>/status — per-process status */
static int procfs_gen_pid_status(uint32_t pid, char *buf, int max) {
    struct process *p = process_get_by_pid(pid);
    if (!p || p->state == PROCESS_UNUSED) return -1;

    int pos = 0;
    proc_str("Name:\t", buf, &pos, max);
    proc_str(p->name ? p->name : "?", buf, &pos, max);
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

    proc_str("Pid:\t", buf, &pos, max);
    proc_u64_to_str(p->pid, buf, &pos, max);
    proc_str("\n", buf, &pos, max);

    proc_str("PPid:\t", buf, &pos, max);
    proc_u64_to_str(p->parent_pid, buf, &pos, max);
    proc_str("\n", buf, &pos, max);

    proc_str("Tgid:\t", buf, &pos, max);
    proc_u64_to_str(p->tgid, buf, &pos, max);
    proc_str("\n", buf, &pos, max);

    proc_str("Uid:\t", buf, &pos, max);
    proc_u64_to_str(p->uid, buf, &pos, max);
    proc_str("\t", buf, &pos, max);
    proc_u64_to_str(p->euid, buf, &pos, max);
    proc_str("\t0\t0\n", buf, &pos, max);

    proc_str("Gid:\t", buf, &pos, max);
    proc_u64_to_str(p->gid, buf, &pos, max);
    proc_str("\t", buf, &pos, max);
    proc_u64_to_str(p->egid, buf, &pos, max);
    proc_str("\t0\t0\n", buf, &pos, max);

    /* Thread count: count processes with same tgid */
    int threads = 0;
    struct process *table = process_get_table();
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (table[i].state != PROCESS_UNUSED && table[i].tgid == p->tgid)
            threads++;
    }
    proc_str("Threads:\t", buf, &pos, max);
    proc_u64_to_str(threads, buf, &pos, max);
    proc_str("\n", buf, &pos, max);

    /* Signal pending/blocked/ignored */
    proc_str("SigQ:\t", buf, &pos, max);
    proc_u64_to_str(__builtin_popcountll(p->pending_signals), buf, &pos, max);
    proc_str("/", buf, &pos, max);
    proc_u64_to_str(PROCESS_SIG_MAX, buf, &pos, max);
    proc_str("\n", buf, &pos, max);

    proc_str("SigPnd:\t", buf, &pos, max);
    proc_u64_to_str(p->pending_signals, buf, &pos, max);
    proc_str("\n", buf, &pos, max);

    proc_str("ShdPnd:\t0\n", buf, &pos, max);

    proc_str("SigBlk:\t", buf, &pos, max);
    proc_u64_to_str(p->sig_mask, buf, &pos, max);
    proc_str("\n", buf, &pos, max);

    proc_str("SigIgn:\t0\n", buf, &pos, max);

    /* Capabilities */
    proc_str("CapInh:\t0000000000000000\n", buf, &pos, max);
    proc_str("CapPrm:\t", buf, &pos, max);
    for (int w = PROCESS_SYSCALL_CAP_WORDS - 1; w >= 0; w--) {
        for (int nib = 15; nib >= 0; nib--) {
            uint8_t nibble = (p->syscall_caps[w] >> (nib * 4)) & 0xF;
            buf[pos++] = nibble < 10 ? '0' + nibble : 'a' + nibble - 10;
            if (pos >= max - 1) break;
        }
    }
    buf[pos] = '\0'; /* don't advance pos yet, just safety */
    proc_str("\n", buf, &pos, max);

    proc_str("CapEff:\t", buf, &pos, max);
    for (int w = PROCESS_SYSCALL_CAP_WORDS - 1; w >= 0; w--) {
        for (int nib = 15; nib >= 0; nib--) {
            uint8_t nibble = (p->syscall_caps[w] >> (nib * 4)) & 0xF;
            buf[pos++] = nibble < 10 ? '0' + nibble : 'a' + nibble - 10;
            if (pos >= max - 1) break;
        }
    }
    buf[pos] = '\0';
    proc_str("\n", buf, &pos, max);

    proc_str("Cpus_allowed:\t", buf, &pos, max);
    proc_u64_to_str(p->cpu_affinity, buf, &pos, max);
    proc_str("\n", buf, &pos, max);

    /* Keep existing fields too */
    proc_str("Priority:\t", buf, &pos, max);
    proc_u64_to_str(p->priority, buf, &pos, max);
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

/* /proc/<pid>/cmdline — command line (from proc_comm) */
static int procfs_gen_pid_cmdline(uint32_t pid, char *buf, int max) {
    struct process *p = process_get_by_pid(pid);
    if (!p || p->state == PROCESS_UNUSED) return -1;

    int pos = 0;
    if (p->proc_comm[0]) {
        proc_str(p->proc_comm, buf, &pos, max);
    } else if (p->name) {
        proc_str(p->name, buf, &pos, max);
    }
    buf[pos] = '\0';
    return pos;
}

/* /proc/<pid>/environ — environment variables */
static int procfs_gen_pid_environ(uint32_t pid, char *buf, int max) {
    struct process *p = process_get_by_pid(pid);
    if (!p || p->state == PROCESS_UNUSED) return -1;

    int pos = 0;
    /* Read environment from user stack (typically at ELF aux vector area) */
    if (p->is_user && p->user_rsp) {
        /* The environment is on the user stack; we access it via user memory.
         * For simplicity, show basic env vars from the process struct. */
        proc_str("HOME=/", buf, &pos, max);
        if (pos < max - 1) buf[pos++] = '\0';
        proc_str("PATH=/bin:/usr/bin", buf, &pos, max);
        if (pos < max - 1) buf[pos++] = '\0';
        if (p->proc_comm[0]) {
            proc_str("_=", buf, &pos, max);
            proc_str(p->proc_comm, buf, &pos, max);
            if (pos < max - 1) buf[pos++] = '\0';
        }
    }
    buf[pos] = '\0';
    return pos;
}

/* /proc/<pid>/io — I/O statistics (standard Linux proc interface) */
static int procfs_gen_pid_io(uint32_t pid, char *buf, int max) {
    struct process *p = process_get_by_pid(pid);
    if (!p || p->state == PROCESS_UNUSED) return -1;

    int pos = 0;
    /* Format matches Linux /proc/PID/io: key: value\n */
    proc_str("rchar: ", buf, &pos, max);
    proc_u64_to_str(p->io_rchar, buf, &pos, max);
    proc_str("\n", buf, &pos, max);
    proc_str("wchar: ", buf, &pos, max);
    proc_u64_to_str(p->io_wchar, buf, &pos, max);
    proc_str("\n", buf, &pos, max);
    proc_str("syscr: ", buf, &pos, max);
    proc_u64_to_str(p->io_syscr, buf, &pos, max);
    proc_str("\n", buf, &pos, max);
    proc_str("syscw: ", buf, &pos, max);
    proc_u64_to_str(p->io_syscw, buf, &pos, max);
    proc_str("\n", buf, &pos, max);
    proc_str("read_bytes: ", buf, &pos, max);
    proc_u64_to_str(p->io_read_bytes, buf, &pos, max);
    proc_str("\n", buf, &pos, max);
    proc_str("write_bytes: ", buf, &pos, max);
    proc_u64_to_str(p->io_write_bytes, buf, &pos, max);
    proc_str("\n", buf, &pos, max);
    proc_str("cancelled_write_bytes: ", buf, &pos, max);
    proc_u64_to_str(p->io_write_bytes, buf, &pos, max);
    proc_str("\n", buf, &pos, max);
    buf[pos] = '\0';
    return pos;
}

/* /proc/<pid>/ns — list namespace types and identifiers (Item 120) */
static int procfs_gen_pid_ns(uint32_t pid, char *buf, int max) {
    struct process *p = process_get_by_pid(pid);
    if (!p || p->state == PROCESS_UNUSED) return -1;

    int pos = 0;
    /* UTS namespace — dynamically compute inode from hostname + pointer */
    {
        uint64_t ns_inode = 0;
        const char *h = (p->ns_hostname[0]) ? p->ns_hostname : "localhost";
        for (int i = 0; h[i]; i++)
            ns_inode = ns_inode * 31 + (uint8_t)h[i];
        ns_inode ^= (uint64_t)(uintptr_t)p;
        ns_inode &= 0x7FFFFFFFFFFFFFFFULL;
        if (ns_inode < 1024) ns_inode += 1024;
        proc_str("ns\t", buf, &pos, max);
        proc_u64_to_str(ns_inode, buf, &pos, max);
        proc_str("\t/uts\n", buf, &pos, max);
    }
    /* PID namespace — always the initial namespace for now */
    proc_str("ns\t4026531836\t/pid\n", buf, &pos, max);
    /* Mount namespace */
    proc_str("ns\t4026531840\t/mnt\n", buf, &pos, max);
    /* Network namespace */
    proc_str("ns\t4026531841\t/net\n", buf, &pos, max);
    /* IPC namespace */
    proc_str("ns\t4026531839\t/ipc\n", buf, &pos, max);
    /* Cgroup namespace */
    {
        uint64_t cg_inode = p->cgroup_ns
            ? cgroup_ns_inode(p->cgroup_ns) : 4026531835ULL;
        proc_str("ns\t", buf, &pos, max);
        proc_u64_to_str(cg_inode, buf, &pos, max);
        proc_str("\t/cgroup\n", buf, &pos, max);
    }
    /* Time namespace */
    proc_str("ns\t4026531834\t/time\n", buf, &pos, max);

    buf[pos] = '\0';
    return pos;
}

/* /proc/<pid>/maps — memory mappings */
static int procfs_gen_pid_maps(uint32_t pid, char *buf, int max) {
    struct process *p = process_get_by_pid(pid);
    if (!p || p->state == PROCESS_UNUSED) return -1;

    int pos = 0;
    /* Report VMA-style memory regions from the process's page table */
    if (p->is_user && p->pml4) {
        /* User code region */
        proc_str("0000000000400000-0000000000800000 r-xp 00000000 00:00 0", buf, &pos, max);
        proc_str("         /init\n", buf, &pos, max);
        /* User data/stack region */
        proc_str("00007ffffffde000-00007ffffffff000 rw-p 00000000 00:00 0", buf, &pos, max);
        proc_str("         [stack]\n", buf, &pos, max);
        /* Heap region */
        proc_str("0000000000800000-0000000000a00000 rw-p 00000000 00:00 0", buf, &pos, max);
        proc_str("         [heap]\n", buf, &pos, max);
        /* vDSO-like region */
        proc_str("00007ffff7ff7000-00007ffff7ffa000 r-xp 00000000 00:00 0", buf, &pos, max);
        proc_str("         [vdso]\n", buf, &pos, max);
    } else {
        /* Kernel thread — hide if kptr_restrict restricts it */
        if (kptr_restrict_check()) {
            proc_str("0000000000000000-0000000000000000 r-xp 00000000 00:00 0", buf, &pos, max);
        } else {
            proc_str("ffffffff80000000-ffffffff80a00000 r-xp 00000000 00:00 0", buf, &pos, max);
        }
        proc_str("         [kernel]\n", buf, &pos, max);
    }
    buf[pos] = '\0';
    return pos;
}

/* /proc/<pid>/smaps — detailed per-VMA memory statistics */
static int procfs_gen_pid_smaps(uint32_t pid, char *buf, int max) {
    struct process *p = process_get_by_pid(pid);
    if (!p || p->state == PROCESS_UNUSED) return -1;

    int pos = 0;

    /* Helper to format a memory region's status line.
     * Per Linux smaps format: one entry per VMA with address range, perms,
     * offset, device, inode, pathname, followed by detail lines. */
    #define SMAPS_REGION(start, end, perms, name) do {                     \
        proc_str(start " " end " " perms " 00000000 00:00 0",             \
                 buf, &pos, max);                                         \
        proc_str("         " name "\n", buf, &pos, max);                  \
    } while(0)

    #define SMAPS_DETAIL(label, fmt, val) do {                             \
        char tmp[48];                                                      \
        proc_str(label, buf, &pos, max);                                   \
        snprintf(tmp, sizeof(tmp), fmt, (unsigned long long)(val));        \
        proc_str(tmp, buf, &pos, max);                                     \
        proc_str(" kB\n", buf, &pos, max);                                 \
    } while(0)

    if (p->is_user && p->pml4) {
        /* ── User code region: 0x400000 - 0x800000 ─────────────── */
        {
            uint64_t rss, dirty, shared;
            rss = vmm_count_user_pages_range(p->pml4,
                        0x400000ULL, 0x800000ULL, &dirty, &shared);
            uint64_t rss_kb  = rss * 4;
            uint64_t dirty_kb = dirty * 4;
            uint64_t shared_kb = shared * 4;
            uint64_t pss_kb = shared_kb > 0 ? shared_kb / 2 : 0; /* approximate */
            uint64_t private_kb = rss_kb > shared_kb ? rss_kb - shared_kb : 0;

            SMAPS_REGION("0000000000400000", "0000000000800000",
                         "r-xp", "/init");
            SMAPS_DETAIL("Rss:               ", "%llu", rss_kb);
            SMAPS_DETAIL("Pss:               ", "%llu", pss_kb);
            SMAPS_DETAIL("Shared_Clean:      ", "%llu", shared_kb);
            SMAPS_DETAIL("Private_Clean:     ", "%llu", private_kb);
            SMAPS_DETAIL("Private_Dirty:     ", "%llu", dirty_kb);
            SMAPS_DETAIL("Referenced:        ", "%llu", rss_kb);
            proc_str("Swap:              0 kB\n", buf, &pos, max);
        }

        /* ── Heap region: 0x800000 - 0xA00000 ──────────────────── */
        {
            uint64_t rss, dirty, shared;
            rss = vmm_count_user_pages_range(p->pml4,
                        0x800000ULL, 0xA00000ULL, &dirty, &shared);
            uint64_t rss_kb  = rss * 4;
            uint64_t dirty_kb = dirty * 4;
            uint64_t shared_kb = shared * 4;
            uint64_t pss_kb = shared_kb > 0 ? shared_kb / 2 : 0;
            uint64_t private_kb = rss_kb > shared_kb ? rss_kb - shared_kb : 0;

            SMAPS_REGION("0000000000800000", "0000000000a00000",
                         "rw-p", "[heap]");
            SMAPS_DETAIL("Rss:               ", "%llu", rss_kb);
            SMAPS_DETAIL("Pss:               ", "%llu", pss_kb);
            SMAPS_DETAIL("Shared_Clean:      ", "%llu", shared_kb);
            SMAPS_DETAIL("Private_Clean:     ", "%llu", private_kb);
            SMAPS_DETAIL("Private_Dirty:     ", "%llu", dirty_kb);
            SMAPS_DETAIL("Referenced:        ", "%llu", rss_kb);
            proc_str("Swap:              0 kB\n", buf, &pos, max);
        }

        /* ── vDSO region: 0x7ffff7ff7000 - 0x7ffff7ffa000 ──────── */
        {
            uint64_t rss, dirty, shared;
            rss = vmm_count_user_pages_range(p->pml4,
                        0x7FFFF7FF7000ULL, 0x7FFFF7FFA000ULL, &dirty, &shared);
            uint64_t rss_kb  = rss * 4;
            uint64_t dirty_kb = dirty * 4;
            uint64_t shared_kb = shared * 4;
            uint64_t pss_kb = shared_kb > 0 ? shared_kb / 2 : 0;
            uint64_t private_kb = rss_kb > shared_kb ? rss_kb - shared_kb : 0;

            SMAPS_REGION("00007ffff7ff7000", "00007ffff7ffa000",
                         "r-xp", "[vdso]");
            SMAPS_DETAIL("Rss:               ", "%llu", rss_kb);
            SMAPS_DETAIL("Pss:               ", "%llu", pss_kb);
            SMAPS_DETAIL("Shared_Clean:      ", "%llu", shared_kb);
            SMAPS_DETAIL("Private_Clean:     ", "%llu", private_kb);
            SMAPS_DETAIL("Private_Dirty:     ", "%llu", dirty_kb);
            SMAPS_DETAIL("Referenced:        ", "%llu", rss_kb);
            proc_str("Swap:              0 kB\n", buf, &pos, max);
        }

        /* ── Stack region: 0x7ffffffde000 - 0x7ffffffff000 ──────── */
        {
            uint64_t rss, dirty, shared;
            rss = vmm_count_user_pages_range(p->pml4,
                        0x7FFFFFFDE000ULL, 0x7FFFFFFFF000ULL, &dirty, &shared);
            uint64_t rss_kb  = rss * 4;
            uint64_t dirty_kb = dirty * 4;
            uint64_t shared_kb = shared * 4;
            uint64_t pss_kb = shared_kb > 0 ? shared_kb / 2 : 0;
            uint64_t private_kb = rss_kb > shared_kb ? rss_kb - shared_kb : 0;

            SMAPS_REGION("00007ffffffde000", "00007ffffffff000",
                         "rw-p", "[stack]");
            SMAPS_DETAIL("Rss:               ", "%llu", rss_kb);
            SMAPS_DETAIL("Pss:               ", "%llu", pss_kb);
            SMAPS_DETAIL("Shared_Clean:      ", "%llu", shared_kb);
            SMAPS_DETAIL("Private_Clean:     ", "%llu", private_kb);
            SMAPS_DETAIL("Private_Dirty:     ", "%llu", dirty_kb);
            SMAPS_DETAIL("Referenced:        ", "%llu", rss_kb);
            proc_str("Swap:              0 kB\n", buf, &pos, max);
        }
    } else {
        /* Kernel thread — show kernel mapping */
        {
            uint64_t rss = pmm_get_used_frames(); /* rough approximation */
            SMAPS_REGION("ffffffff80000000", "ffffffff80a00000",
                         "r-xp", "[kernel]");
            SMAPS_DETAIL("Rss:               ", "%llu", rss * 4);
            SMAPS_DETAIL("Pss:               ", "%llu", rss * 4);
            proc_str("Shared_Clean:      0 kB\n", buf, &pos, max);
            SMAPS_DETAIL("Private_Clean:     ", "%llu", rss * 4);
            proc_str("Private_Dirty:     0 kB\n", buf, &pos, max);
            SMAPS_DETAIL("Referenced:        ", "%llu", rss * 4);
            proc_str("Swap:              0 kB\n", buf, &pos, max);
        }
    }

    buf[pos] = '\0';
    return pos;
}

/* /proc/<pid>/numa_maps — NUMA memory policy per VMA (Item 133)
 *
 * Shows NUMA node distribution for each memory-mapped region.
 * Format (Linux-compatible):
 *   <start>-<end> <policy> <mapping> N<node>=<pages> ...
 *
 * For the current simple VMA model we report the home NUMA node
 * for each known region based on the process's home_node.
 */
static int procfs_gen_pid_numa_maps(uint32_t pid, char *buf, int max)
{
    struct process *p = process_get_by_pid(pid);
    if (!p || p->state == PROCESS_UNUSED) return -1;

    int pos = 0;
    (void)max;
    int node = p->home_node;

    /* If NUMA topology is not available, default to node 0 */
    if (node < 0 || node >= 8)
        node = 0;

    /* Helper: emit one VMA line in numa_maps format */
    #define NUMA_MAPS_VMA(start, end, perms, name, pages, node_id) do {       \
        char pages_str[16];                                                  \
        char node_str[16];                                                   \
        proc_str(start "-" end " ", buf, &pos, max);                         \
        proc_str("default ", buf, &pos, max);                                \
        proc_str(name, buf, &pos, max);                                      \
        proc_str(" ", buf, &pos, max);                                       \
        proc_str(perms, buf, &pos, max);                                     \
        snprintf(node_str, sizeof(node_str), " N%u=",                       \
                 (unsigned int)(node_id));                                    \
        proc_str(node_str, buf, &pos, max);                                  \
        snprintf(pages_str, sizeof(pages_str), "%llu",                       \
                 (unsigned long long)(pages));                                \
        proc_str(pages_str, buf, &pos, max);                                 \
        proc_str("\n", buf, &pos, max);                                      \
    } while(0)

    if (p->is_user && p->pml4) {
        /* User code: 0x400000 - 0x800000 */
        uint64_t rss, dirty, shared;
        rss = vmm_count_user_pages_range(p->pml4,
                    0x400000ULL, 0x800000ULL, &dirty, &shared);
        NUMA_MAPS_VMA("0000000000400000", "0000000000800000",
                      "r-xp", "file=/init", rss, node);

        /* Heap: 0x800000 - 0xA00000 */
        rss = vmm_count_user_pages_range(p->pml4,
                    0x800000ULL, 0xA00000ULL, &dirty, &shared);
        NUMA_MAPS_VMA("0000000000800000", "0000000000a00000",
                      "rw-p", "heap", rss, node);

        /* vDSO: 0x7ffff7ff7000 - 0x7ffff7ffa000 */
        rss = vmm_count_user_pages_range(p->pml4,
                    0x7FFFF7FF7000ULL, 0x7FFFF7FFA000ULL, &dirty, &shared);
        NUMA_MAPS_VMA("00007ffff7ff7000", "00007ffff7ffa000",
                      "r-xp", "map=/vdso", rss, node);

        /* Stack: 0x7ffffffde000 - 0x7ffffffff000 */
        rss = vmm_count_user_pages_range(p->pml4,
                    0x7FFFFFFDE000ULL, 0x7FFFFFFFF000ULL, &dirty, &shared);
        NUMA_MAPS_VMA("00007ffffffde000", "00007ffffffff000",
                      "rw-p", "stack", rss, node);
    } else {
        /* Kernel thread — show kernel mapping only */
        uint64_t kpages = pmm_get_used_frames();
        NUMA_MAPS_VMA("ffffffff80000000", "ffffffff80a00000",
                      "r-xp", "map=/kernel", kpages, node);
    }

    buf[pos] = '\0';
    return pos;
}

/* /proc/<pid>/limits — per-process resource limits */
static int procfs_gen_pid_limits(uint32_t pid, char *buf, int max) {
    struct process *p = process_get_by_pid(pid);
    if (!p || p->state == PROCESS_UNUSED) return -1;

    static const struct {
        int         resource;
        const char *name;
        const char *units;
    } rlim_info[] = {
        { RLIMIT_CPU,     "Max cpu time",        "seconds"   },
        { RLIMIT_FSIZE,   "Max file size",       "bytes"     },
        { RLIMIT_DATA,    "Max data size",        "bytes"     },
        { RLIMIT_STACK,   "Max stack size",       "bytes"     },
        { RLIMIT_CORE,    "Max core file size",   "bytes"     },
        { RLIMIT_RSS,     "Max resident set",     "bytes"     },
        { RLIMIT_NPROC,   "Max processes",        "processes" },
        { RLIMIT_NOFILE,  "Max open files",       "files"     },
        { RLIMIT_MEMLOCK, "Max locked memory",    "bytes"     },
        { RLIMIT_AS,      "Max address space",    "bytes"     },
    };

    int pos = 0;
    for (size_t i = 0; i < sizeof(rlim_info) / sizeof(rlim_info[0]); i++) {
        int r = rlim_info[i].resource;
        if (r < 0 || r >= RLIMIT_NLIMITS) continue;

        uint64_t cur = p->rlim_cur[r];
        uint64_t max_val = p->rlim_max[r];

        /* Format: "Name                 Soft Limit     Hard Limit     Units\n" */
        proc_str(rlim_info[i].name, buf, &pos, max);

        /* Pad to column 24 */
        int pad = 24 - (int)strlen(rlim_info[i].name);
        while (pad > 0 && pos < max - 1) { buf[pos++] = ' '; pad--; }

        if (cur == RLIM_INFINITY) {
            proc_str("unlimited", buf, &pos, max);
        } else {
            char tmp[24];
            int ti = 0;
            uint64_t v = cur;
            if (v == 0) { tmp[ti++] = '0'; }
            else { char rev[24]; int ri = 0;
                   while (v) { rev[ri++] = '0' + (int)(v % 10); v /= 10; }
                   while (ri > 0) tmp[ti++] = rev[--ri]; }
            tmp[ti] = '\0';
            proc_str(tmp, buf, &pos, max);
        }

        /* Pad to column 44 */
        {
            /* Find where we are in the line */
            int line_start = pos;
            while (line_start > 0 && buf[line_start - 1] != '\n') line_start--;
            int col = pos - line_start;
            pad = 44 - col;
            while (pad > 0 && pos < max - 1) { buf[pos++] = ' '; pad--; }
        }

        if (max_val == RLIM_INFINITY) {
            proc_str("unlimited", buf, &pos, max);
        } else {
            char tmp[24];
            int ti = 0;
            uint64_t v = max_val;
            if (v == 0) { tmp[ti++] = '0'; }
            else { char rev[24]; int ri = 0;
                   while (v) { rev[ri++] = '0' + (int)(v % 10); v /= 10; }
                   while (ri > 0) tmp[ti++] = rev[--ri]; }
            tmp[ti] = '\0';
            proc_str(tmp, buf, &pos, max);
        }

        /* Pad to column 64 then units */
        {
            int line_start = pos;
            while (line_start > 0 && buf[line_start - 1] != '\n') line_start--;
            int col = pos - line_start;
            pad = 64 - col;
            while (pad > 0 && pos < max - 1) { buf[pos++] = ' '; pad--; }
        }

        proc_str(rlim_info[i].units, buf, &pos, max);
        proc_str("\n", buf, &pos, max);

        if (pos >= max - 1) break;
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
    } else if (strcmp(path, "/proc/interrupts") == 0) {
        len = procfs_gen_interrupts(buf, (int)max_size);
    } else if (strcmp(path, "/proc/meminfo") == 0) {
        len = procfs_gen_meminfo(buf, (int)max_size);
    } else if (strcmp(path, "/proc/cpuinfo") == 0) {
        len = procfs_gen_cpuinfo(buf, (int)max_size);
    } else if (strcmp(path, "/proc/version") == 0) {
        len = procfs_gen_version(buf, (int)max_size);
    } else if (strcmp(path, "/proc/config.gz") == 0) {
        uint32_t gz_size = 0;
        const void *gz_data = config_gz_get_data(&gz_size);
        if (gz_data && gz_size > 0) {
            uint32_t copy = (gz_size < max_size) ? gz_size : max_size;
            memcpy(buf, gz_data, copy);
            *out_size = copy;
            return 0;
        }
        return -1;
    } else if (strcmp(path, "/proc/net/arp") == 0) {
        len = procfs_gen_arp(buf, (int)max_size);
    } else if (strcmp(path, "/proc/net/route") == 0) {
        len = procfs_gen_route(buf, (int)max_size);
    } else if (strcmp(path, "/proc/net/dev") == 0) {
        len = procfs_gen_net_dev(buf, (int)max_size);
    } else if (strcmp(path, "/proc/net/tcp") == 0) {
        len = procfs_gen_net_tcp(buf, (int)max_size);
    } else if (strcmp(path, "/proc/mounts") == 0) {
        len = procfs_gen_mounts(buf, (int)max_size);
    } else if (strcmp(path, "/proc/stat") == 0) {
        len = procfs_gen_stat(buf, (int)max_size);
    } else if (strcmp(path, "/proc/loadavg") == 0) {
        len = procfs_gen_loadavg(buf, (int)max_size);
    } else if (strcmp(path, "/proc/vmstat") == 0) {
        len = procfs_gen_vmstat(buf, (int)max_size);
    } else if (strcmp(path, "/proc/slabinfo") == 0) {
        len = procfs_gen_slabinfo(buf, (int)max_size);
    } else if (strcmp(path, "/proc/filesystems") == 0) {
        len = procfs_gen_filesystems(buf, (int)max_size);
    } else if (strcmp(path, "/proc/modules") == 0) {
        len = procfs_gen_modules(buf, (int)max_size);
    } else if (strcmp(path, "/proc/self") == 0) {
        /* Redirect to status of current process */
        struct process *proc = process_get_current();
        if (proc)
            len = procfs_gen_pid_status(proc->pid, buf, (int)max_size);
        else
            return -1;
    } else if (strcmp(path, "/proc/self/exe") == 0) {
        /* Return path to current process's executable */
        struct process *proc = process_get_current();
        if (!proc || !proc->exe_path[0])
            return -1;
        len = (int)strlen(proc->exe_path);
        if (len > (int)max_size - 1) len = (int)max_size - 1;
        memcpy(buf, proc->exe_path, (size_t)len);
        buf[len] = '\n';
        len++;
    } else if (strncmp(path, "/proc/sys/kernel/", 17) == 0) {
        /* Sysctl read */
        len = sysctl_read(path + 17, buf, (int)max_size);
        if (len < 0) return -1;
    } else if (strcmp(path, "/proc/sys") == 0) {
        /* /proc/sys is a directory */
        return -1;
    } else if (strcmp(path, "/proc/sys/kernel") == 0) {
        /* /proc/sys/kernel is a directory */
        return -1;
    } else if (strcmp(path, "/proc/pressure") == 0) {
        /* /proc/pressure is a directory listing */
        return -1;
    } else if (strcmp(path, "/proc/pressure/cpu") == 0) {
        len = psi_gen_proc_file(PSI_RES_CPU, buf, (int)max_size);
        if (len < 0) return -1;
    } else if (strcmp(path, "/proc/pressure/memory") == 0) {
        len = psi_gen_proc_file(PSI_RES_MEMORY, buf, (int)max_size);
        if (len < 0) return -1;
    } else if (strcmp(path, "/proc/pressure/io") == 0) {
        len = psi_gen_proc_file(PSI_RES_IO, buf, (int)max_size);
        if (len < 0) return -1;
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
        } else if (got && strcmp(p, "/environ") == 0) {
            len = procfs_gen_pid_environ(pid, buf, (int)max_size);
            if (len < 0) return -1;
        } else if (got && strcmp(p, "/maps") == 0) {
            len = procfs_gen_pid_maps(pid, buf, (int)max_size);
            if (len < 0) return -1;
        } else if (got && strcmp(p, "/smaps") == 0) {
            len = procfs_gen_pid_smaps(pid, buf, (int)max_size);
            if (len < 0) return -1;
        } else if (got && strcmp(p, "/numa_maps") == 0) {
            len = procfs_gen_pid_numa_maps(pid, buf, (int)max_size);
            if (len < 0) return -1;
        } else if (got && strcmp(p, "/limits") == 0) {
            len = procfs_gen_pid_limits(pid, buf, (int)max_size);
            if (len < 0) return -1;
        } else if (got && strcmp(p, "/io") == 0) {
            len = procfs_gen_pid_io(pid, buf, (int)max_size);
            if (len < 0) return -1;
        } else if (got && strcmp(p, "/cgroup") == 0) {
            /* /proc/<pid>/cgroup — show cgroup path, virtualized by namespace (Item 117) */
            struct process *proc = process_get_by_pid(pid);
            if (!proc || proc->state == PROCESS_UNUSED) return -1;
            const char *full_path = "/sys/fs/cgroup";
            char vpath[128];
            if (proc->cgroup_ns) {
                cgroup_ns_get_path(proc->cgroup_ns, full_path, vpath, sizeof(vpath));
            } else {
                strncpy(vpath, full_path, sizeof(vpath) - 1);
                vpath[sizeof(vpath) - 1] = '\0';
            }
            len = snprintf(buf, (size_t)max_size, "0::%s\n", vpath);
            if (len < 0) return -1;
        } else if (got && strcmp(p, "/ns") == 0) {
            /* /proc/<pid>/ns — list available namespace types */
            len = procfs_gen_pid_ns(pid, buf, (int)max_size);
            if (len < 0) return -1;
        } else if (got && strncmp(p, "/ns/", 4) == 0) {
            /* /proc/<pid>/ns/<type> — read namespace identifier (Linux: "type:[inode]") */
            struct process *proc = process_get_by_pid(pid);
            if (!proc || proc->state == PROCESS_UNUSED) return -1;
            const char *ns_type = p + 4;
            if (strcmp(ns_type, "uts") == 0) {
                /* Compute a namespace inode from the hostname + process pointer */
                uint64_t ns_inode = 0;
                const char *h = (proc->ns_hostname[0]) ? proc->ns_hostname : "localhost";
                for (int i = 0; h[i]; i++)
                    ns_inode = ns_inode * 31 + (uint8_t)h[i];
                ns_inode ^= (uint64_t)(uintptr_t)proc;
                ns_inode &= 0x7FFFFFFFFFFFFFFFULL;
                if (ns_inode < 1024) ns_inode += 1024;
                len = snprintf(buf, (size_t)max_size, "uts:[%llu]\n",
                               (unsigned long long)ns_inode);
            } else if (strcmp(ns_type, "pid") == 0) {
                len = snprintf(buf, (size_t)max_size, "pid:[4026531836]\n");
            } else if (strcmp(ns_type, "mnt") == 0) {
                len = snprintf(buf, (size_t)max_size, "mnt:[4026531840]\n");
            } else if (strcmp(ns_type, "net") == 0) {
                len = snprintf(buf, (size_t)max_size, "net:[4026531841]\n");
            } else if (strcmp(ns_type, "ipc") == 0) {
                len = snprintf(buf, (size_t)max_size, "ipc:[4026531839]\n");
            } else if (strcmp(ns_type, "cgroup") == 0) {
                uint64_t cg_inode = proc->cgroup_ns
                    ? cgroup_ns_inode(proc->cgroup_ns) : 4026531835ULL;
                len = snprintf(buf, (size_t)max_size, "cgroup:[%llu]\n",
                               (unsigned long long)cg_inode);
            } else if (strcmp(ns_type, "time") == 0) {
                len = snprintf(buf, (size_t)max_size, "time:[4026531834]\n");
            } else {
                return -1;
            }
            if (len < 0) return -1;
        } else if (got && strncmp(p, "/fd/", 4) == 0) {
            /* /proc/<pid>/fd/<N> — read symlink target (file path) */
            struct process *proc = process_get_by_pid(pid);
            if (!proc || proc->state == PROCESS_UNUSED) return -1;
            const char *fd_str = p + 4;
            int fd_num = 0;
            while (*fd_str >= '0' && *fd_str <= '9')
                fd_num = fd_num * 10 + (int)(*fd_str++ - '0');
            if (*fd_str != '\0' || fd_num < 0 || fd_num >= PROCESS_FD_MAX)
                return -1;
            if (!proc->fd_table[fd_num].used)
                return -1;
            len = (int)strlen(proc->fd_table[fd_num].path);
            if (len > (int)max_size - 1)
                len = (int)max_size - 1;
            memcpy(buf, proc->fd_table[fd_num].path, (size_t)len);
            buf[len] = '\0';
        } else {
            return -1;
        }
    }

    if (out_size) *out_size = (uint32_t)len;
    return 0;
}

static int procfs_write(void *priv, const char *path, const void *data, uint32_t size) {
    (void)priv;
    const char *buf = (const char *)data;

    /* Sysctl write */
    if (strncmp(path, "/proc/sys/kernel/", 17) == 0) {
        if (sysctl_write(path + 17, buf, (int)size) < 0) return -1;
        return 0;
    }

    /* /proc/sysrq-trigger — write a command character to trigger SysRq */
    if (strcmp(path, "/proc/sysrq-trigger") == 0) {
        if (size < 1) return -1;
        char cmd = buf[0];
        /* Accept lowercase letters */
        if (cmd >= 'a' && cmd <= 'z') {
            sysrq_handle(cmd);
            return 0;
        }
        return -1;
    }

    return -1;
}

static int procfs_stat(void *priv, const char *path, struct vfs_stat *st) {
    (void)priv;
    /* /proc itself is a directory */
    if (strcmp(path, "/proc") == 0) {
        st->type = 2; st->size = 0; return 0;
    }
    /* Known files */
    if (strcmp(path, "/proc/uptime") == 0 ||
        strcmp(path, "/proc/interrupts") == 0 ||
        strcmp(path, "/proc/meminfo") == 0 ||
        strcmp(path, "/proc/cpuinfo") == 0 ||
        strcmp(path, "/proc/version") == 0 ||
        strcmp(path, "/proc/config.gz") == 0 ||
        strcmp(path, "/proc/net/arp") == 0 ||
        strcmp(path, "/proc/net/route") == 0 ||
        strcmp(path, "/proc/net/dev") == 0 ||
        strcmp(path, "/proc/net/tcp") == 0 ||
        strcmp(path, "/proc/mounts") == 0 ||
        strcmp(path, "/proc/stat") == 0 ||
        strcmp(path, "/proc/loadavg") == 0 ||
        strcmp(path, "/proc/vmstat") == 0 ||
        strcmp(path, "/proc/slabinfo") == 0 ||
        strcmp(path, "/proc/sysrq-trigger") == 0) {
        st->type = 1; st->size = 256; return 0;
    }
    /* /proc/filesystems */
    if (strcmp(path, "/proc/filesystems") == 0) {
        st->type = 1; st->size = 512; return 0;
    }
    /* /proc/sys/kernel/ files */
    if (strncmp(path, "/proc/sys/kernel/", 17) == 0) {
        st->type = 1; st->size = 256; return 0;
    }
    /* /proc/sys and /proc/sys/kernel directories */
    if (strcmp(path, "/proc/sys") == 0 || strcmp(path, "/proc/sys/kernel") == 0) {
        st->type = 2; st->size = 0; return 0;
    }
    /* /proc/self is a symlink to /proc/<pid>/ — return directory type */
    if (strcmp(path, "/proc/self") == 0) {
        st->type = 2; st->size = 0; return 0;
    }
    /* /proc/self/exe — executable path of current process */
    if (strcmp(path, "/proc/self/exe") == 0) {
        st->type = 1; st->size = 256; return 0;
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
            st->type = 2; st->size = 0; return 0;  /* directory */
        }
    }
    /* /proc/<pid>/fd/<N> — individual fd symlink entries */
    if (got && strncmp(p, "/fd/", 4) == 0) {
        struct process *proc = process_get_by_pid(pid);
        if (proc && proc->state != PROCESS_UNUSED) {
            const char *fd_str = p + 4;
            int fd_num = 0;
            while (*fd_str >= '0' && *fd_str <= '9')
                fd_num = fd_num * 10 + (int)(*fd_str++ - '0');
            if (*fd_str == '\0' && fd_num >= 0 && fd_num < PROCESS_FD_MAX &&
                proc->fd_table[fd_num].used) {
                st->type = 1; st->size = 64; return 0;
            }
        }
    }
    if (got && strcmp(p, "/cmdline") == 0) {
        struct process *proc = process_get_by_pid(pid);
        if (proc && proc->state != PROCESS_UNUSED) {
            st->type = 1; st->size = 256; return 0;
        }
    }
    if (got && strcmp(p, "/environ") == 0) {
        struct process *proc = process_get_by_pid(pid);
        if (proc && proc->state != PROCESS_UNUSED) {
            st->type = 1; st->size = 256; return 0;
        }
    }
    if (got && strcmp(p, "/maps") == 0) {
        struct process *proc = process_get_by_pid(pid);
        if (proc && proc->state != PROCESS_UNUSED) {
            st->type = 1; st->size = 512; return 0;
        }
    }
    if (got && strcmp(p, "/smaps") == 0) {
        struct process *proc = process_get_by_pid(pid);
        if (proc && proc->state != PROCESS_UNUSED) {
            st->type = 1; st->size = 1024; return 0;
        }
    }
    if (got && strcmp(p, "/limits") == 0) {
        struct process *proc = process_get_by_pid(pid);
        if (proc && proc->state != PROCESS_UNUSED) {
            st->type = 1; st->size = 512; return 0;
        }
    }
    if (got && strcmp(p, "/io") == 0) {
        struct process *proc = process_get_by_pid(pid);
        if (proc && proc->state != PROCESS_UNUSED) {
            st->type = 1; st->size = 512; return 0;
        }
    }
    if (got && strcmp(p, "/numa_maps") == 0) {
        struct process *proc = process_get_by_pid(pid);
        if (proc && proc->state != PROCESS_UNUSED) {
            st->type = 1; st->size = 512; return 0;
        }
    }
    return -1;
}

static int procfs_readdir(void *priv, const char *path) {
    (void)priv;
    if (strcmp(path, "/proc") == 0) {
        kprintf("uptime\nmeminfo\ncpuinfo\nversion\nconfig.gz\nself\nstat\nloadavg\nnet\nmounts\n");
        /* Also list active PIDs */
        struct process *table = process_get_table();
        struct process *caller = process_get_current();
        for (int i = 0; i < PROCESS_MAX; i++) {
            if (table[i].state != PROCESS_UNUSED) {
                if (!caller || process_can_see(caller, &table[i]))
                    kprintf("%lu\n", (unsigned long)table[i].pid);
            }
        }
        return 0;
    }

    /* /proc/<pid>/fd/ — list open file descriptors */
    if (strncmp(path, "/proc/", 6) == 0) {
        const char *p = path + 6;
        uint32_t pid = 0; int got = 0;
        while (*p >= '0' && *p <= '9') { pid = pid * 10 + (uint32_t)(*p - '0'); p++; got = 1; }
        if (got && strcmp(p, "/fd") == 0) {
            struct process *proc = process_get_by_pid(pid);
            if (!proc || proc->state == PROCESS_UNUSED) return -1;
            for (int i = 0; i < PROCESS_FD_MAX; i++) {
                if (proc->fd_table[i].used)
                    kprintf("%d\n", i);
            }
            return 0;
        }
    }
    return -1;
}

struct vfs_ops procfs_ops = {
    .read    = procfs_read,
    .write   = procfs_write,
    .stat    = procfs_stat,
    .create  = NULL,
    .unlink  = NULL,
    .readdir = procfs_readdir,
};

/* ── procfs_init — Initialise and mount /proc ──────────────────────────
 *
 * Called from the built-in init path (vfs_init in kernel.c).
 * When built as a loadable module, this function is called from
 * init_module() instead.
 *
 * Mounts /proc with the procfs VFS operations so that /proc/cpuinfo,
 * /proc/meminfo, /proc/uptime, /proc/PID/, and other virtual files
 * become accessible to userspace.
 */
void procfs_init(void)
{
    if (vfs_mount("/proc", &procfs_ops, NULL) == 0) {
        kprintf("[OK] procfs mounted on /proc\n");
    } else {
        kprintf("[!!] procfs mount failed\n");
    }
}

#ifdef MODULE
#include "module.h"

/* Module entry point — called by the module ELF loader on insmod */
int init_module(void)
{
    procfs_init();
    return 0;
}

/* Module exit point — called by the module ELF loader on rmmod */
void cleanup_module(void)
{
    kprintf("[procfs] Module unloaded\n");
}

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Hermes OS");
MODULE_DESCRIPTION("procfs — /proc virtual filesystem (loadable module)");
#endif /* MODULE */

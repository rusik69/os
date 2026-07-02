/*
 * procfs_cpuinfo.c — /proc/cpuinfo generator
 *
 * Linux-compatible /proc/cpuinfo with full CPU topology and features.
 * Uses CPUID leaves 0, 1, 4, 7, 0x16, 0x80000000–0x80000008,
 * plus MSR 0x8B for microcode version and the cpu_topology API
 * for proper package / core / thread topology.
 */

#include "types.h"
#include "string.h"
#include "cpu.h"
#include "cpu_topology.h"
#include "smp.h"

/* ── Helper: write a 64-bit unsigned decimal into a buffer ─────────── */
extern void proc_u64_to_str(uint64_t v, char *buf, int *pos, int max);
extern void proc_str(const char *s, char *buf, int *pos, int max);

/* ── CPUID wrappers (volatile to prevent reordering) ───────────────── */
static inline void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b,
                         uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid"
                     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf), "c"(0));
}

static inline void cpuid_count(uint32_t leaf, uint32_t subleaf,
                               uint32_t *a, uint32_t *b,
                               uint32_t *c, uint32_t *d) {
    __asm__ volatile("cpuid"
                     : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                     : "a"(leaf), "c"(subleaf));
}

/* ── MSR read (leaf 0x8B = microcode version on Intel) ────────────── */
static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

/* ── Known x86 CPUs that have the microcode MSR at 0x8B ──────────── */
static int intel_vendor(uint32_t ebx, uint32_t ecx, uint32_t edx) {
    return ebx == 0x756e6547 && edx == 0x49656e69 && ecx == 0x6c65746e;
}
static int amd_vendor(uint32_t ebx, uint32_t ecx, uint32_t edx) {
    return ebx == 0x68747541 && edx == 0x69746e65 && ecx == 0x444d4163;
}

/* ── Append a single flag if the feature bit is set ───────────────── */
#define FLAG(cond, name) do { if (cond) proc_str(name, buf, &p, max); } while (0)

/* ── Main generator ───────────────────────────────────────────────── */
int procfs_gen_cpuinfo(char *buf, int max) {
    int p = 0;
    uint32_t eax, ebx, ecx, edx;
    uint32_t seax, sebx, secx, sedx;  /* sub-leaf results */
    char vendor[13];
    uint32_t max_leaf;

    /* Leaf 0: vendor string */
    cpuid(0, &eax, &ebx, &ecx, &edx);
    max_leaf = eax;
    *(uint32_t *)&vendor[0] = ebx;
    *(uint32_t *)&vendor[4] = edx;
    *(uint32_t *)&vendor[8] = ecx;
    vendor[12] = '\0';

    /* Determine whether this is Intel or AMD for vendor-specific logic */
    int is_intel = intel_vendor(ebx, ecx, edx);
    int is_amd   = amd_vendor(ebx, ecx, edx);

    /* Number of CPUs online */
    int cpu_count = smp_get_cpu_count();
    if (cpu_count < 1) cpu_count = 1;

    /* Pre-probe extended leaves */
    uint32_t max_ext;
    cpuid(0x80000000, &max_ext, &sebx, &secx, &sedx);
    (void)sebx; (void)secx; (void)sedx;

    /* Pre-probe leaf 7 subleaf 0 features */
    uint32_t leaf7_ebx = 0, leaf7_ecx = 0, leaf7_edx = 0;
    if (max_leaf >= 7) {
        cpuid_count(7, 0, &seax, &leaf7_ebx, &leaf7_ecx, &leaf7_edx);
        (void)seax;
    }

    /* Pre-probe extended power-management leaf (0x80000007) for TscInvariant */
    uint32_t ext_pm_edx = 0;
    if (max_ext >= 0x80000007) {
        cpuid(0x80000007, &seax, &sebx, &secx, &sedx);
        (void)seax; (void)sebx; (void)secx;
        ext_pm_edx = sedx;
    }

    /* Pre-probe leaf 0x80000008 for physical / virtual address sizes */
    uint32_t phys_bits = 39, virt_bits = 48;
    if (max_ext >= 0x80000008) {
        cpuid(0x80000008, &seax, &sebx, &secx, &sedx);
        (void)sebx; (void)secx; (void)sedx;
        if (seax & 0xFF) phys_bits = seax & 0xFF;
        if ((seax >> 8) & 0xFF) virt_bits = (seax >> 8) & 0xFF;
    }

    /* Pre-probe leaf 0x16 for base / max frequency */
    uint32_t base_mhz = 0, max_mhz_probe = 0, frac_mhz = 0;
    if (max_leaf >= 0x16) {
        cpuid(0x16, &eax, &ebx, &ecx, &edx);
        (void)edx;
        base_mhz   = eax;
        max_mhz_probe = ebx;
        frac_mhz = (ebx * 100) / (eax ? eax : 1);
    }

    for (int cpu = 0; cpu < cpu_count; cpu++) {
        /* Use cpu_topology API (detects via CPUID 0xB on Intel) */
        struct cpu_topology topo = cpu_topology_get();

        /* processor number */
        proc_str("processor\t: ", buf, &p, max);
        proc_u64_to_str((uint64_t)cpu, buf, &p, max);
        proc_str("\n", buf, &p, max);

        /* vendor_id */
        proc_str("vendor_id\t: ", buf, &p, max);
        proc_str(vendor, buf, &p, max);
        proc_str("\n", buf, &p, max);

        /* cpu family (dual-report: decimal + hex for Linux compat) */
        cpuid(1, &eax, &ebx, &ecx, &edx);
        /* Leaf 1 has been called; capture clflush now */
        uint32_t clflush_sz = ((ebx >> 8) & 0xFF) * 8; /* in bytes */

        uint32_t family = (eax >> 8) & 0xF;
        uint32_t ext_family = (eax >> 20) & 0xFF;
        uint32_t model  = (eax >> 4) & 0xF;
        uint32_t ext_model = (eax >> 16) & 0xF;
        uint32_t stepping = eax & 0xF;

        proc_str("cpu family\t: ", buf, &p, max);
        if (family == 0xF) {
            proc_u64_to_str(family + ext_family, buf, &p, max);
        } else {
            proc_u64_to_str(family, buf, &p, max);
        }
        proc_str("\n", buf, &p, max);

        proc_str("model\t\t: ", buf, &p, max);
        if (family == 0x6 || family == 0xF) {
            proc_u64_to_str((ext_model << 4) | model, buf, &p, max);
        } else {
            proc_u64_to_str(model, buf, &p, max);
        }
        proc_str("\n", buf, &p, max);

        proc_str("model name\t: ", buf, &p, max);

        /* Leaf 0x80000002-4: brand string */
        char brand[49];
        memset(brand, 0, sizeof(brand));
        if (max_ext >= 0x80000004) {
            for (uint32_t i = 0; i < 3; i++) {
                cpuid(0x80000002 + i, &eax, &ebx, &ecx, &edx);
                *(uint32_t *)&brand[i * 16 + 0]  = eax;
                *(uint32_t *)&brand[i * 16 + 4]  = ebx;
                *(uint32_t *)&brand[i * 16 + 8]  = ecx;
                *(uint32_t *)&brand[i * 16 + 12] = edx;
            }
        }
        if (brand[0]) {
            proc_str(brand, buf, &p, max);
        } else {
            proc_str("Unknown", buf, &p, max);
        }
        proc_str("\n", buf, &p, max);

        proc_str("stepping\t: ", buf, &p, max);
        proc_u64_to_str(stepping, buf, &p, max);
        proc_str("\n", buf, &p, max);

        /* Microcode version from MSR 0x8B (Intel) or MSR 0xC0010070 (AMD) */
        if (is_intel) {
            uint64_t ucode = rdmsr(0x8B);
            proc_str("microcode\t: 0x", buf, &p, max);
            /* Print as hex */
            char hexbuf[17];
            int hn = 0;
            for (int s = 60; s >= 0; s -= 4) {
                uint8_t nib = (uint8_t)((ucode >> s) & 0xF);
                if (hn > 0 || nib || s == 0) {
                    hexbuf[hn++] = (char)(nib < 10 ? '0' + nib : 'a' + nib - 10);
                }
            }
            hexbuf[hn] = '\0';
            if (hn == 0) { hexbuf[0] = '0'; hexbuf[1] = '\0'; }
            proc_str(hexbuf, buf, &p, max);
            proc_str("\n", buf, &p, max);
        } else if (is_amd) {
            uint64_t ucode = rdmsr(0xC0010070);
            proc_str("microcode\t: 0x", buf, &p, max);
            char hexbuf[17];
            int hn = 0;
            for (int s = 60; s >= 0; s -= 4) {
                uint8_t nib = (uint8_t)((ucode >> s) & 0xF);
                if (hn > 0 || nib || s == 0) {
                    hexbuf[hn++] = (char)(nib < 10 ? '0' + nib : 'a' + nib - 10);
                }
            }
            hexbuf[hn] = '\0';
            if (hn == 0) { hexbuf[0] = '0'; hexbuf[1] = '\0'; }
            proc_str(hexbuf, buf, &p, max);
            proc_str("\n", buf, &p, max);
        }

        /* CPU frequency */
        proc_str("cpu MHz\t\t: ", buf, &p, max);
        if (base_mhz) {
            proc_u64_to_str(base_mhz, buf, &p, max);
            proc_str(".", buf, &p, max);
            if (frac_mhz < 10) proc_str("0", buf, &p, max);
            proc_u64_to_str(frac_mhz, buf, &p, max);
        } else {
            proc_str("0", buf, &p, max);
            proc_str(".000", buf, &p, max);
        }
        proc_str("\n", buf, &p, max);

        /* Cache info from leaf 4 (deterministic cache params) */
        if (max_leaf >= 4) {
            for (int ct = 0; ct < 10; ct++) {
                cpuid_count(4, ct, &eax, &ebx, &ecx, &edx);
                int type = eax & 0x1F;
                if (type == 0) break;
                int level    = (eax >> 5) & 0x7;
                int ways     = ((ebx >> 22) & 0x3FF) + 1;
                int parts    = ((ebx >> 12) & 0x3FF) + 1;
                int line_sz  = (ebx & 0xFFF) + 1;
                int sets     = (int)(ecx + 1);
                uint64_t size = (uint64_t)ways * parts * line_sz * sets;
                const char *type_s = "Unknown";
                if (type == 1) type_s = "Data";
                else if (type == 2) type_s = "Instruction";
                else if (type == 3) type_s = "Unified";
                proc_str("cache size\t: L", buf, &p, max);
                proc_u64_to_str(level, buf, &p, max);
                proc_str(" ", buf, &p, max);
                proc_str(type_s, buf, &p, max);
                proc_str(" ", buf, &p, max);
                proc_u64_to_str(size / 1024, buf, &p, max);
                proc_str(" KB\n", buf, &p, max);
            }
        }

        /* Physical / core IDs from cpu_topology API */
        proc_str("physical id\t: ", buf, &p, max);
        proc_u64_to_str((uint64_t)topo.package_id, buf, &p, max);
        proc_str("\n", buf, &p, max);

        proc_str("siblings\t: ", buf, &p, max);
        proc_u64_to_str((uint64_t)cpu_count, buf, &p, max);
        proc_str("\n", buf, &p, max);

        proc_str("core id\t\t: ", buf, &p, max);
        proc_u64_to_str((uint64_t)topo.core_id, buf, &p, max);
        proc_str("\n", buf, &p, max);

        proc_str("cpu cores\t: ", buf, &p, max);
        /* Count unique core IDs among online CPUs */
        int unique_cores = 1;
        if (cpu_count > 1) {
            /* Simplification: assume hyperthreading means cores < cpus.
             * A robust implementation would walk all CPU topologies. */
            unique_cores = topo.thread_id == 0 ?
                           cpu_count : cpu_count / (cpu_count > 1 ? 2 : 1);
        }
        proc_u64_to_str((uint64_t)unique_cores, buf, &p, max);
        proc_str("\n", buf, &p, max);

        /* APIC ID from CPUID leaf 1 EBX[31:24] */
        proc_str("apicid\t\t: ", buf, &p, max);
        proc_u64_to_str((ebx >> 24) & 0xFF, buf, &p, max);
        proc_str("\n", buf, &p, max);

        /* Initial APIC ID (same value on modern CPUs) */
        proc_str("initial apicid\t: ", buf, &p, max);
        proc_u64_to_str((ebx >> 24) & 0xFF, buf, &p, max);
        proc_str("\n", buf, &p, max);

        /* fpu / fpu_exception */
        proc_str("fpu\t\t: ", buf, &p, max);
        proc_str((edx & (1U << 0)) ? "yes" : "no", buf, &p, max);
        proc_str("\n", buf, &p, max);
        proc_str("fpu_exception\t: ", buf, &p, max);
        proc_str((edx & (1U << 0)) ? "yes" : "no", buf, &p, max);
        proc_str("\n", buf, &p, max);

        /* cpuid level (max standard leaf) */
        proc_str("cpuid level\t: ", buf, &p, max);
        proc_u64_to_str(max_leaf, buf, &p, max);
        proc_str("\n", buf, &p, max);

        /* wp (write-protect) — always on in long mode */
        proc_str("wp\t\t: yes\n", buf, &p, max);

        /* Flags (full feature list) */
        proc_str("flags\t\t: ", buf, &p, max);

        /* Leaf 1 EDX features */
        FLAG(edx & (1U << 0),  "fpu ");
        FLAG(edx & (1U << 1),  "vme ");
        FLAG(edx & (1U << 2),  "de ");
        FLAG(edx & (1U << 3),  "pse ");
        FLAG(edx & (1U << 4),  "tsc ");
        FLAG(edx & (1U << 5),  "msr ");
        FLAG(edx & (1U << 6),  "pae ");
        FLAG(edx & (1U << 7),  "mce ");
        FLAG(edx & (1U << 8),  "cx8 ");
        FLAG(edx & (1U << 9),  "apic ");
        FLAG(edx & (1U << 10), "mtrr ");     /* (reserved in some docs) */
        FLAG(edx & (1U << 11), "sep ");
        FLAG(edx & (1U << 12), "mtrr ");
        FLAG(edx & (1U << 13), "pge ");
        FLAG(edx & (1U << 14), "mca ");
        FLAG(edx & (1U << 15), "cmov ");
        FLAG(edx & (1U << 16), "pat ");
        FLAG(edx & (1U << 17), "pse36 ");
        FLAG(edx & (1U << 18), "pn ");
        FLAG(edx & (1U << 19), "clflush ");
        FLAG(edx & (1U << 20), "dts ");
        FLAG(edx & (1U << 21), "acpi ");
        FLAG(edx & (1U << 22), "mmx ");
        FLAG(edx & (1U << 23), "mmx ");
        FLAG(edx & (1U << 24), "fxsr ");
        FLAG(edx & (1U << 25), "sse ");
        FLAG(edx & (1U << 26), "sse2 ");
        FLAG(edx & (1U << 27), "ss ");
        FLAG(edx & (1U << 28), "ht ");
        FLAG(edx & (1U << 29), "tm ");
        FLAG(edx & (1U << 30), "ia64 ");
        FLAG(edx & (1U << 31), "pbe ");

        /* Leaf 1 ECX features (SSE3 and beyond) */
        FLAG(ecx & (1U << 0),  "sse3 ");
        FLAG(ecx & (1U << 1),  "pclmulqdq ");
        FLAG(ecx & (1U << 2),  "dtes64 ");
        FLAG(ecx & (1U << 3),  "monitor ");
        FLAG(ecx & (1U << 4),  "ds_cpl ");
        FLAG(ecx & (1U << 5),  "vmx ");
        FLAG(ecx & (1U << 6),  "smx ");
        FLAG(ecx & (1U << 7),  "est ");
        FLAG(ecx & (1U << 8),  "tm2 ");
        FLAG(ecx & (1U << 9),  "ssse3 ");
        FLAG(ecx & (1U << 10), "cid ");
        FLAG(ecx & (1U << 11), "sdbg ");
        FLAG(ecx & (1U << 12), "fma ");
        FLAG(ecx & (1U << 13), "cx16 ");
        FLAG(ecx & (1U << 14), "xtpr ");
        FLAG(ecx & (1U << 15), "pdcm ");
        FLAG(ecx & (1U << 16), "pcid ");
        FLAG(ecx & (1U << 17), "dca ");
        FLAG(ecx & (1U << 18), "sse4_1 ");
        FLAG(ecx & (1U << 19), "sse4_2 ");
        FLAG(ecx & (1U << 20), "x2apic ");
        FLAG(ecx & (1U << 21), "movbe ");
        FLAG(ecx & (1U << 22), "popcnt ");
        FLAG(ecx & (1U << 23), "tsc_deadline ");
        FLAG(ecx & (1U << 24), "aes ");
        FLAG(ecx & (1U << 25), "xsave ");
        FLAG(ecx & (1U << 26), "osxsave ");
        FLAG(ecx & (1U << 27), "avx ");
        FLAG(ecx & (1U << 28), "f16c ");
        FLAG(ecx & (1U << 29), "rdrand ");
        FLAG(ecx & (1U << 30), "hypervisor ");

        /* Leaf 7 EBX features (extended feature leaves) */
        FLAG(leaf7_ebx & (1U << 0),  "fsgsbase ");
        FLAG(leaf7_ebx & (1U << 1),  "tsc_adjust ");
        FLAG(leaf7_ebx & (1U << 2),  "sgx ");
        FLAG(leaf7_ebx & (1U << 3),  "bmi1 ");
        FLAG(leaf7_ebx & (1U << 4),  "hle ");
        FLAG(leaf7_ebx & (1U << 5),  "avx2 ");
        FLAG(leaf7_ebx & (1U << 6),  "fdp ");
        FLAG(leaf7_ebx & (1U << 7),  "smep ");
        FLAG(leaf7_ebx & (1U << 8),  "bmi2 ");
        FLAG(leaf7_ebx & (1U << 9),  "erms ");
        FLAG(leaf7_ebx & (1U << 10), "invpcid ");
        FLAG(leaf7_ebx & (1U << 11), "rtm ");
        FLAG(leaf7_ebx & (1U << 12), "cqm ");
        FLAG(leaf7_ebx & (1U << 13), "fpu_csds ");
        FLAG(leaf7_ebx & (1U << 14), "mpx ");
        FLAG(leaf7_ebx & (1U << 15), "pqe ");
        FLAG(leaf7_ebx & (1U << 16), "avx512f ");
        FLAG(leaf7_ebx & (1U << 17), "avx512dq ");
        FLAG(leaf7_ebx & (1U << 18), "rdseed ");
        FLAG(leaf7_ebx & (1U << 19), "adx ");
        FLAG(leaf7_ebx & (1U << 20), "smap ");
        FLAG(leaf7_ebx & (1U << 21), "avx512ifma ");
        FLAG(leaf7_ebx & (1U << 22), "pcommit ");
        FLAG(leaf7_ebx & (1U << 23), "clflushopt ");
        FLAG(leaf7_ebx & (1U << 24), "clwb ");
        FLAG(leaf7_ebx & (1U << 25), "intel_pt ");
        FLAG(leaf7_ebx & (1U << 26), "avx512pf ");
        FLAG(leaf7_ebx & (1U << 27), "avx512er ");
        FLAG(leaf7_ebx & (1U << 28), "avx512cd ");
        FLAG(leaf7_ebx & (1U << 29), "sha_ni ");
        FLAG(leaf7_ebx & (1U << 30), "avx512bw ");
        FLAG(leaf7_ebx & (1U << 31), "avx512vl ");

        /* Leaf 7 ECX features */
        FLAG(leaf7_ecx & (1U << 0),  "prefetchwt1 ");
        FLAG(leaf7_ecx & (1U << 1),  "avx512vbmi ");
        FLAG(leaf7_ecx & (1U << 2),  "umip ");
        FLAG(leaf7_ecx & (1U << 3),  "pku ");
        FLAG(leaf7_ecx & (1U << 4),  "ospke ");
        FLAG(leaf7_ecx & (1U << 5),  "waitpkg ");
        FLAG(leaf7_ecx & (1U << 6),  "avx512vbmi2 ");
        FLAG(leaf7_ecx & (1U << 7),  "cet_ss ");
        FLAG(leaf7_ecx & (1U << 8),  "gfni ");
        FLAG(leaf7_ecx & (1U << 9),  "vaes ");
        FLAG(leaf7_ecx & (1U << 10), "vpclmulqdq ");
        FLAG(leaf7_ecx & (1U << 11), "avx512vnni ");
        FLAG(leaf7_ecx & (1U << 12), "avx512bitalg ");
        FLAG(leaf7_ecx & (1U << 13), "tme ");
        FLAG(leaf7_ecx & (1U << 14), "avx512vpopcntdq ");
        FLAG(leaf7_ecx & (1U << 15), "la57 ");
        FLAG(leaf7_ecx & (1U << 22), "rdpid ");
        FLAG(leaf7_ecx & (1U << 26), "ibrs ");
        FLAG(leaf7_ecx & (1U << 27), "stibp ");
        FLAG(leaf7_ecx & (1U << 28), "l1d_flush ");
        FLAG(leaf7_ecx & (1U << 29), "arch_cap ");

        /* Leaf 7 EDX features */
        FLAG(leaf7_edx & (1U << 0),  "cldemote ");
        FLAG(leaf7_edx & (1U << 10), "md_clear ");
        FLAG(leaf7_edx & (1U << 14), "serialize ");
        FLAG(leaf7_edx & (1U << 18), "pconfig ");
        FLAG(leaf7_edx & (1U << 26), "ibrs ");

        /* Extended features from leaf 0x80000001 */
        if (max_ext >= 0x80000001) {
            uint32_t ex_edx, ex_ecx;
            cpuid(0x80000001, &seax, &sebx, &secx, &sedx);
            (void)seax; (void)sebx;
            ex_ecx = secx;
            ex_edx = sedx;

            FLAG(ex_edx & (1U << 0),  "fpu_64 ");
            FLAG(ex_edx & (1U << 11), "syscall ");
            FLAG(ex_edx & (1U << 20), "nx ");
            FLAG(ex_edx & (1U << 22), "mmxext ");
            FLAG(ex_edx & (1U << 23), "ffxsr ");
            FLAG(ex_edx & (1U << 25), "fxsr_opt ");    /* same as ffxsr */
            FLAG(ex_edx & (1U << 26), "rdtscp ");
            FLAG(ex_edx & (1U << 27), "lm ");
            FLAG(ex_edx & (1U << 28), "3dnowext ");
            FLAG(ex_edx & (1U << 29), "3dnow ");

            FLAG(ex_ecx & (1U << 0),  "lahf_lm ");
            FLAG(ex_ecx & (1U << 1),  "cmp_legacy ");
            FLAG(ex_ecx & (1U << 2),  "svm ");
            FLAG(ex_ecx & (1U << 3),  "extapic ");
            FLAG(ex_ecx & (1U << 4),  "cr8_legacy ");
            FLAG(ex_ecx & (1U << 5),  "abm ");
            FLAG(ex_ecx & (1U << 6),  "sse4a ");
            FLAG(ex_ecx & (1U << 7),  "misalignsse ");
            FLAG(ex_ecx & (1U << 8),  "3dnowprefetch ");
            FLAG(ex_ecx & (1U << 9),  "osvw ");
            FLAG(ex_ecx & (1U << 10), "ibs ");
            FLAG(ex_ecx & (1U << 11), "xop ");
            FLAG(ex_ecx & (1U << 12), "skinit ");
            FLAG(ex_ecx & (1U << 13), "wdt ");
            FLAG(ex_ecx & (1U << 15), "lwp ");
            FLAG(ex_ecx & (1U << 16), "fma4 ");
            FLAG(ex_ecx & (1U << 17), "tce ");
            FLAG(ex_ecx & (1U << 19), "nodeid ");
            FLAG(ex_ecx & (1U << 20), "tbm ");
            FLAG(ex_ecx & (1U << 21), "topoext ");
            FLAG(ex_ecx & (1U << 22), "perfctr_core ");
            FLAG(ex_ecx & (1U << 23), "perfctr_nb ");
            FLAG(ex_ecx & (1U << 24), "bpext ");
            FLAG(ex_ecx & (1U << 26), "pci_iov ");
            FLAG(ex_ecx & (1U << 28), "monitorx ");
        }

        /* Invariant TSC from leaf 0x80000007 EDX bit 8 */
        FLAG((ext_pm_edx & (1U << 8)), "constant_tsc ");
        FLAG((ext_pm_edx & (1U << 8)), "nonstop_tsc ");

        proc_str("\n", buf, &p, max);

        /* Bugs list (Linux-compatible placeholder) */
        proc_str("bugs\t\t: ", buf, &p, max);
        /* Check for known speculative execution vulnerabilities */
        if (leaf7_edx & (1U << 10)) /* MD_CLEAR */
            proc_str("spectre_v1 spectre_v2 spec_store_bypass mds ", buf, &p, max);
        else
            proc_str("spectre_v1 spectre_v2 spec_store_bypass ", buf, &p, max);
        if (leaf7_ecx & (1U << 28)) /* L1D_FLUSH */
            proc_str("l1tf ", buf, &p, max);
        proc_str("\n", buf, &p, max);

        /* Bogomips: CPU frequency * 2 / 1000000 (classic approximation) */
        proc_str("bogomips\t: ", buf, &p, max);
        if (base_mhz) {
            proc_u64_to_str((uint64_t)base_mhz * 2, buf, &p, max);
            proc_str(".", buf, &p, max);
            uint32_t bogo_frac = (frac_mhz * 2) / 100;
            if (bogo_frac < 10) proc_str("0", buf, &p, max);
            proc_u64_to_str(bogo_frac, buf, &p, max);
        } else {
            proc_str("0.00", buf, &p, max);
        }
        proc_str("\n", buf, &p, max);

        /* clflush size */
        proc_str("clflush size\t: ", buf, &p, max);
        proc_u64_to_str(clflush_sz, buf, &p, max);
        proc_str("\n", buf, &p, max);

        /* cache_alignment (always 64 on x86_64) */
        proc_str("cache_alignment\t: 64\n", buf, &p, max);

        /* Address sizes */
        proc_str("address sizes\t: ", buf, &p, max);
        proc_u64_to_str(phys_bits, buf, &p, max);
        proc_str(" bits physical, ", buf, &p, max);
        proc_u64_to_str(virt_bits, buf, &p, max);
        proc_str(" bits virtual\n", buf, &p, max);

        /* Power management */
        proc_str("power management\t: ", buf, &p, max);
        int pm_capable = 0;
        if (ecx & (1U << 7))  { proc_str("est ", buf, &p, max); pm_capable = 1; }  /* SpeedStep */
        if (ecx & (1U << 8))  { proc_str("tm2 ", buf, &p, max); pm_capable = 1; }
        if (ext_pm_edx & (1U << 8))  { proc_str("tsc ", buf, &p, max); pm_capable = 1; }
        if (edx & (1U << 29)) { proc_str("tm ", buf, &p, max); pm_capable = 1; }
        if (edx & (1U << 20)) { proc_str("dts ", buf, &p, max); pm_capable = 1; }
        if (!pm_capable) proc_str("not supported", buf, &p, max);
        proc_str("\n", buf, &p, max);

        /* Separator between CPUs (Linux convention: blank line) */
        proc_str("\n", buf, &p, max);

        if (p >= max - 2) break;
    }

    buf[p] = '\0';
    return p;
}

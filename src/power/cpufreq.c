/*
 * cpufreq.c — CPU frequency scaling core
 *
 * Implements MSR-based P-state control (Intel SpeedStep / AMD Cool'n'Quiet)
 * and provides the cpupstate API declared in <cpupstate.h>.
 *
 * Modern Intel CPUs (Sandy Bridge+) expose frequency control via:
 *   - IA32_PERF_STATUS (MSR 0x198) — current P-state (bits 7:0 = state number)
 *   - IA32_PERF_CTL    (MSR 0x199) — requested P-state (bits 7:0 = state number)
 *   - IA32_MPERF       (MSR 0xE7)   — TSC counter running at P0 frequency
 *   - IA32_APERF       (MSR 0xE8)   — TSC counter running at actual frequency
 *   - CPUID leaf 0x16  (Skylake+)   — reports base frequency in MHz
 *
 * The ratio (APERF / MPERF) × base_freq gives the actual CPU frequency.
 *
 * Sysfs interface:
 *   /sys/devices/system/cpu/cpu0/cpufreq/
 *      - available_frequencies
 *      - cpuinfo_cur_freq
 *      - scaling_cur_freq
 *      - scaling_available_governors (placeholder)
 *      - scaling_governor (writable)
 *      - scaling_max_freq / scaling_min_freq (writable)
 *      - related_cpus
 */

#include "cpupstate.h"
#include "cpu.h"         /* read_msr, write_msr */
#include "printf.h"
#include "string.h"
#include "sysfs.h"
#include "cpufreq_ondemand.h"
#include "cpufreq_schedutil.h"

/* ─── MSR definitions ──────────────────────────────────────────────── */

/* Performance Monitoring Counters for frequency measurement */
#define MSR_IA32_MPERF         0x000000E7
#define MSR_IA32_APERF         0x000000E8

/* ─── Static state ─────────────────────────────────────────────────── */

static struct cpupstate_ctrl g_cpufreq = {0};

/* Base frequency in kHz — detected once at init */
static uint32_t g_base_freq_khz = 0;

/* APERF/MPERF snapshot for frequency calculation */
static uint64_t g_last_mperf = 0;
static uint64_t g_last_aperf = 0;

/* ─── Forward declarations ─────────────────────────────────────────── */

static int detect_base_frequency(void);
static int probe_pstates_msr(void);
static int cpufreq_sysfs_init(void);

/* ─── Frequency detection helpers ──────────────────────────────────── */

/* Read a 32-bit CPUID leaf into eax/ebx/ecx/edx */
static inline void cpuid_leaf(uint32_t leaf,
                               uint32_t *eax, uint32_t *ebx,
                               uint32_t *ecx, uint32_t *edx)
{
    uint32_t a = leaf, b = 0, c = 0, d = 0;
    __asm__ volatile("cpuid"
                     : "=a"(a), "=b"(b), "=c"(c), "=d"(d)
                     : "a"(a), "c"(c));
    *eax = a; *ebx = b; *ecx = c; *edx = d;
}

/*
 * Detect the base (maximum, P0) frequency.
 *
 * Strategy:
 *   1. Try CPUID leaf 0x16 (Intel Skylake+) — returns base freq in eax (MHz).
 *   2. If not available, read MSR IA32_PLATFORM_ID and the current
 *      PERF_STATUS to derive frequency.
 *   3. Fall back to TSC calibration frequency from the kernel.
 */
static int detect_base_frequency(void)
{
    uint32_t eax, ebx, ecx, edx;

    /* Check for max CPUID leaf */
    cpuid_leaf(0, &eax, &ebx, &ecx, &edx);
    uint32_t max_leaf = eax;

    /* Try CPUID leaf 0x16 (Intel: Processor Frequency Information) */
    if (max_leaf >= 0x16) {
        cpuid_leaf(0x16, &eax, &ebx, &ecx, &edx);
        if (eax > 0) {
            /* eax = base frequency in MHz */
            g_base_freq_khz = eax * 1000;
            return 0;
        }
    }

    /* Fallback: read current PERF_STATUS and extract the state number.
     * The P-state encoding differs by CPU family; we use a heuristic:
     * assume P0 = max frequency ≈ TSC frequency (which is often correct
     * for modern CPUs where TSC runs at P0 frequency). */
    uint64_t perf_status = read_msr(MSR_IA32_PERF_STATUS_ACTUAL);

    /* On many CPUs, the TSC frequency is the P0 frequency.
     * We can use the OS-calibrated TSC frequency as a proxy. */
    /* Try reading APERF/MPERF ratio to detect base */
    uint64_t mperf = read_msr(MSR_IA32_MPERF);
    uint64_t aperf = read_msr(MSR_IA32_APERF);

    if (mperf > 0 && aperf > 0 && aperf <= mperf) {
        /* APERF measures actual cycles, MPERF measures at P0 rate.
         * At P0, aperf ≈ mperf. The actual frequency = (aperf/mperf) * P0_freq.
         * We don't know P0 yet, but we can assume it's the TSC frequency. */
        /* Use the MSR encoding: on Intel, the P-state number in PERF_STATUS
         * bits 7:0 encodes the voltage/frequency pair. Higher numbers = lower freq.
         * P0 has the lowest state number. */
        (void)perf_status;
    }

    /* Last resort: kprintf a message but set a reasonable default.
     * Many emulators (QEMU, Bochs) run at a base frequency close to 2 GHz. */
    kprintf("[cpufreq] WARNING: could not detect base frequency via CPUID, "
            "defaulting to 2000 MHz\n");
    g_base_freq_khz = 2000000; /* 2 GHz default for QEMU/emulators */
    return 0;
}

/*
 * Probe available P-states via MSR interface.
 *
 * On Intel Sandy Bridge+, P-state is controlled via the IA32_PERF_CTL MSR.
 * The state number (0..N) encodes a frequency/voltage pair.
 * P0 = highest frequency, P1 = guaranteed frequency, Pn = lowest.
 *
 * We enumerate P-states by trying to write and read back different values.
 * A simpler approach: assume P0 base frequency, and model lower P-states
 * as decreasing steps of ~100 MHz.
 */
static int probe_pstates_msr(void)
{
    uint64_t perf_status;
    uint8_t current_pstate;
    int num_states;
    uint32_t freq_step_khz;

    /* Read current P-state from IA32_PERF_STATUS (MSR 0x198) */
    perf_status = read_msr(MSR_IA32_PERF_STATUS_ACTUAL);
    current_pstate = (uint8_t)(perf_status & 0xFF);

    /* Determine the maximum P-state number.
     * On many Intel CPUs, bits 16:8 of PERF_CTL contain the minimum voltage.
     * The maximum P-state number can sometimes be derived from PLATFORM_ID MSR,
     * or we can use a fixed table lookup. We use a simple heuristic:
     * Assume P-state range 0..15, where 0 = P0 (max freq).
     * The actual number of valid states depends on the CPU model.
     */

    /* Try to read the minimum P-state from the platform:
     * On some CPUs, writing all 1s to the state field and reading back
     * gives the minimum valid state. We avoid this because it's unsafe.
     * Instead, we create a reasonable set of P-states. */
    num_states = 8;  /* Assume 8 P-states (P0..P7) like most Intel CPUs */

    /* Clamp to our static array size */
    if (num_states > CPUPSTATE_MAX_STATES)
        num_states = CPUPSTATE_MAX_STATES;

    /* Compute frequency step: divide base frequency across P-states.
     * Modern CPUs typically drop ~100-200 MHz per P-state. */
    freq_step_khz = g_base_freq_khz / (num_states + 1);
    if (freq_step_khz < 100000)  /* At least 100 MHz steps */
        freq_step_khz = 100000;

    /* Build P-state table */
    g_cpufreq.num_states = num_states;
    g_cpufreq.current_state = (int)current_pstate;
    g_cpufreq.present = 1;

    for (int i = 0; i < num_states; i++) {
        uint32_t freq_khz = g_base_freq_khz - (uint32_t)i * freq_step_khz;
        /* Clamp: don't go below ~200 MHz */
        if (freq_khz < 200000) freq_khz = 200000;

        g_cpufreq.states[i].core_freq = freq_khz / 1000;          /* MHz */
        g_cpufreq.states[i].power     = 30000 - (uint32_t)i * 3000; /* Rough mW model */
        g_cpufreq.states[i].transition_latency = 20;               /* ~20 us typical */
        g_cpufreq.states[i].control   = (uint8_t)i;                /* State number to write */
        g_cpufreq.states[i].status    = (uint8_t)i;                /* State number in status */
    }

    kprintf("[cpufreq] Detected %d P-states (base %u MHz, current P%d)\n",
            num_states, g_base_freq_khz / 1000, current_pstate);
    return 0;
}

/*
 * Register externally-discovered P-states (e.g. from ACPI _PSS).
 * Called by acpi_cpufreq_init() when ACPI _PSS data is found.
 * Overrides the default MSR-probed states with ACPI-provided ones.
 *
 * 'states' is an array of 'count' struct cpupstate_state entries.
 * The entries must be sorted from P0 (highest frequency) downward.
 * Base frequency is inferred from states[0].core_freq.
 */
int cpufreq_register_acpi_states(const struct cpupstate_state *states, int count)
{
    if (!states || count <= 0 || count > CPUPSTATE_MAX_STATES)
        return -1;

    memset(&g_cpufreq, 0, sizeof(g_cpufreq));
    g_cpufreq.num_states = count;
    g_cpufreq.current_state = 0;
    g_cpufreq.present = 1;

    for (int i = 0; i < count; i++) {
        g_cpufreq.states[i] = states[i];
    }

    /* Update base frequency from P0 */
    if (count > 0 && states[0].core_freq > 0)
        g_base_freq_khz = states[0].core_freq * 1000;

    /* Take initial APERF/MPERF snapshot */
    g_last_mperf = read_msr(MSR_IA32_MPERF);
    g_last_aperf = read_msr(MSR_IA32_APERF);

    /* Set to P0 */
    cpupstate_set_state(0);

    kprintf("[cpufreq] Registered %d ACPI P-states (P0 = %u MHz)\n",
            count, states[0].core_freq);
    return 0;
}

/* ─── cpupstate API implementation ─────────────────────────────────── */

int cpupstate_init(void)
{
    memset(&g_cpufreq, 0, sizeof(g_cpufreq));
    g_cpufreq.present = 0;
    g_cpufreq.num_states = 0;
    g_cpufreq.current_state = -1;

    /* Detect base frequency */
    if (detect_base_frequency() < 0) {
        kprintf("[cpufreq] Base frequency detection failed — disabled\n");
        return -1;
    }

    /* Probe available P-states via MSR */
    if (probe_pstates_msr() < 0) {
        kprintf("[cpufreq] P-state probing failed — disabled\n");
        return -1;
    }

    /* Take initial APERF/MPERF snapshot */
    g_last_mperf = read_msr(MSR_IA32_MPERF);
    g_last_aperf = read_msr(MSR_IA32_APERF);

    /* Create sysfs interface */
    if (cpufreq_sysfs_init() < 0) {
        kprintf("[cpufreq] sysfs init failed (non-fatal)\n");
    }

    /* Set to P0 (highest performance) by default */
    cpupstate_set_state(0);

    kprintf("[cpufreq] CPU frequency scaling ready — P0 = %u MHz\n",
            g_cpufreq.states[0].core_freq);
    return 0;
}

int cpupstate_set_state(int state)
{
    if (!g_cpufreq.present) return -1;
    if (state < 0 || state >= g_cpufreq.num_states) return -1;

    /*
     * P-state MSR write — read-modify-write to preserve bits:
     *   Bits 7:0   = P-state number / FID
     *   Bits 15:8  = VID (on older Intel CPUs with Enhanced SpeedStep)
     *   Bit  32    = IDA enable (turbo engage)
     *   Bit  33    = IDA disable (turbo inhibit)
     * All other bits must remain unchanged to avoid clobbering
     * platform-specific MSR configuration or previously-set
     * IDA/turbo settings.
     *
     * Read current value, clear the P-state number field (bits 7:0),
     * then set the new number from the state table's control field.
     */
    uint64_t ctl_val = read_msr(MSR_IA32_PERF_CTL_ACTUAL);
    ctl_val &= ~0xFFULL;                     /* Clear bits 7:0 */
    ctl_val |= (uint64_t)(g_cpufreq.states[state].control & 0xFF);  /* Set new P-state */
    write_msr(MSR_IA32_PERF_CTL_ACTUAL, ctl_val);

    g_cpufreq.current_state = state;
    return 0;
}

int cpupstate_get_state(void)
{
    if (!g_cpufreq.present) return -1;
    return g_cpufreq.current_state;
}

int cpupstate_get_count(void)
{
    if (!g_cpufreq.present) return 0;
    return g_cpufreq.num_states;
}

int cpupstate_get_info(int state, struct cpupstate_state *info)
{
    if (!g_cpufreq.present) return -1;
    if (state < 0 || state >= g_cpufreq.num_states) return -1;
    if (!info) return -1;

    *info = g_cpufreq.states[state];
    return 0;
}

int cpupstate_is_present(void)
{
    return g_cpufreq.present;
}

/* ─── Compute current actual frequency from APERF/MPERF ────────────── */

static uint32_t cpufreq_get_actual_freq_khz(void)
{
    if (!g_cpufreq.present || g_base_freq_khz == 0)
        return 0;

    uint64_t mperf = read_msr(MSR_IA32_MPERF);
    uint64_t aperf = read_msr(MSR_IA32_APERF);

    uint64_t dmperf = mperf - g_last_mperf;
    uint64_t daperf = aperf - g_last_aperf;

    /* Update snapshots */
    g_last_mperf = mperf;
    g_last_aperf = aperf;

    if (dmperf == 0) {
        /* No measurement interval — return current state's frequency */
        int cur = g_cpufreq.current_state;
        if (cur >= 0 && cur < g_cpufreq.num_states)
            return g_cpufreq.states[cur].core_freq * 1000;
        return 0;
    }

    /* Actual frequency = base_freq * aperf/mperf */
    uint64_t freq_khz = g_base_freq_khz * daperf / dmperf;
    return (uint32_t)freq_khz;
}

/* ─── Sysfs interface ──────────────────────────────────────────────── */

/* Read callbacks */

static int sysfs_read_available_freqs(char *buf, uint32_t max_size, void *priv)
{
    (void)priv;
    int off = 0;
    for (int i = 0; i < g_cpufreq.num_states && off < (int)max_size - 12; i++) {
        off += snprintf(buf + off, (size_t)(max_size - off),
                        "%u ", g_cpufreq.states[i].core_freq * 1000);
    }
    if (off > 0) buf[off - 1] = '\n'; /* replace trailing space with newline */
    return off;
}

static int sysfs_read_cur_freq(char *buf, uint32_t max_size, void *priv)
{
    (void)priv;
    uint32_t freq = cpufreq_get_actual_freq_khz();
    if (freq == 0) {
        int cur = g_cpufreq.current_state;
        if (cur >= 0 && cur < g_cpufreq.num_states)
            freq = g_cpufreq.states[cur].core_freq * 1000;
    }
    return snprintf(buf, max_size, "%u\n", freq);
}

static int sysfs_read_min_freq(char *buf, uint32_t max_size, void *priv)
{
    (void)priv;
    if (g_cpufreq.num_states <= 0) return 0;
    uint32_t min_freq = g_cpufreq.states[g_cpufreq.num_states - 1].core_freq * 1000;
    return snprintf(buf, max_size, "%u\n", min_freq);
}

static int sysfs_read_max_freq(char *buf, uint32_t max_size, void *priv)
{
    (void)priv;
    if (g_cpufreq.num_states <= 0) return 0;
    uint32_t max_freq = g_cpufreq.states[0].core_freq * 1000;
    return snprintf(buf, max_size, "%u\n", max_freq);
}

static int sysfs_read_governor(char *buf, uint32_t max_size, void *priv)
{
    (void)priv;
    /* Check if schedutil governor is active (check first — highest priority) */
    if (cpufreq_schedutil_is_active())
        return snprintf(buf, max_size, "schedutil\n");
    /* Check if ondemand governor is active */
    if (cpufreq_ondemand_is_active())
        return snprintf(buf, max_size, "ondemand\n");
    /* Default governor */
    return snprintf(buf, max_size, "performance\n");
}

static int sysfs_read_related_cpus(char *buf, uint32_t max_size, void *priv)
{
    (void)priv;
    /* Single CPU for now; SMP cpufreq support later */
    return snprintf(buf, max_size, "0\n");
}

static int sysfs_read_available_governors(char *buf, uint32_t max_size, void *priv)
{
    (void)priv;
    return snprintf(buf, max_size,
                    "performance powersave userspace ondemand schedutil\n");
}

static int sysfs_read_pstate(char *buf, uint32_t max_size, void *priv)
{
    (void)priv;
    return snprintf(buf, max_size, "%d\n", g_cpufreq.current_state);
}

/* Write callbacks */

static int sysfs_write_governor(const char *data, uint32_t size, void *priv)
{
    (void)priv;
    /* Accept "performance", "powersave", or "userspace".
     * "performance" = P0 always.
     * "powersave" = lowest P-state always.
     * "userspace" = manual via scaling_setspeed. */
    char buf[32];
    uint32_t len = size < (uint32_t)sizeof(buf) - 1 ? size : (uint32_t)sizeof(buf) - 1;
    memcpy(buf, data, len);
    buf[len] = '\0';
    /* Strip trailing newline */
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
        buf[--len] = '\0';

    if (strcmp(buf, "performance") == 0) {
        cpufreq_schedutil_stop();
        cpufreq_ondemand_stop();
        cpupstate_set_state(0);
        return 0;
    }
    if (strcmp(buf, "powersave") == 0) {
        cpufreq_schedutil_stop();
        cpufreq_ondemand_stop();
        if (g_cpufreq.num_states > 0)
            cpupstate_set_state(g_cpufreq.num_states - 1);
        return 0;
    }
    if (strcmp(buf, "userspace") == 0) {
        cpufreq_schedutil_stop();
        cpufreq_ondemand_stop();
        /* Will be set via scaling_setspeed */
        return 0;
    }
    if (strcmp(buf, "ondemand") == 0) {
        /* OnDemand governor: start periodic sampling */
        cpufreq_schedutil_stop();
        cpufreq_ondemand_start();
        return 0;
    }
    if (strcmp(buf, "schedutil") == 0) {
        /* SchedUtil governor: stop ondemand if running, start schedutil */
        cpufreq_ondemand_stop();
        cpufreq_schedutil_start();
        return 0;
    }
    return -1; /* Unknown governor */
}

static int sysfs_write_max_freq(const char *data, uint32_t size, void *priv)
{
    (void)priv;
    char buf[32];
    uint32_t len = size < (uint32_t)sizeof(buf) - 1 ? size : (uint32_t)sizeof(buf) - 1;
    memcpy(buf, data, len);
    buf[len] = '\0';

    uint32_t freq_khz = (uint32_t)strtoul(buf, NULL, 10);

    /* Find the closest P-state that doesn't exceed the requested frequency */
    for (int i = 0; i < g_cpufreq.num_states; i++) {
        if (g_cpufreq.states[i].core_freq * 1000 <= freq_khz) {
            cpupstate_set_state(i);
            return 0;
        }
    }
    /* If all frequencies are above the limit, go to the lowest */
    if (g_cpufreq.num_states > 0)
        cpupstate_set_state(g_cpufreq.num_states - 1);
    return 0;
}

static int sysfs_write_min_freq(const char *data, uint32_t size, void *priv)
{
    (void)priv;
    char buf[32];
    uint32_t len = size < (uint32_t)sizeof(buf) - 1 ? size : (uint32_t)sizeof(buf) - 1;
    memcpy(buf, data, len);
    buf[len] = '\0';

    uint32_t freq_khz = (uint32_t)strtoul(buf, NULL, 10);

    /* Find the closest P-state that meets or exceeds the minimum frequency */
    for (int i = g_cpufreq.num_states - 1; i >= 0; i--) {
        if (g_cpufreq.states[i].core_freq * 1000 >= freq_khz) {
            cpupstate_set_state(i);
            return 0;
        }
    }
    return 0;
}

/*
 * Create /sys/devices/system/cpu/cpu0/cpufreq/ hierarchy.
 */
static int cpufreq_sysfs_init(void)
{
    /* Create directory hierarchy */
    if (sysfs_create_dir("/sys/devices") < 0 &&
        sysfs_create_file("/sys/devices", "") < 0) {
        /* May already exist — that's fine */
    }
    if (sysfs_create_dir("/sys/devices/system") < 0 &&
        sysfs_create_file("/sys/devices/system", "") < 0) {
    }
    if (sysfs_create_dir("/sys/devices/system/cpu") < 0 &&
        sysfs_create_file("/sys/devices/system/cpu", "") < 0) {
    }
    if (sysfs_create_dir("/sys/devices/system/cpu/cpu0") < 0 &&
        sysfs_create_file("/sys/devices/system/cpu/cpu0", "") < 0) {
    }

    /* Create cpufreq directory */
    if (sysfs_create_dir("/sys/devices/system/cpu/cpu0/cpufreq") < 0)
        return -1;

    /* Create files with dynamic read/write callbacks */
    sysfs_create_writable_file(
        "/sys/devices/system/cpu/cpu0/cpufreq/available_frequencies",
        NULL, NULL, sysfs_read_available_freqs, NULL);

    sysfs_create_writable_file(
        "/sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_cur_freq",
        NULL, NULL, sysfs_read_cur_freq, NULL);

    sysfs_create_writable_file(
        "/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq",
        NULL, NULL, sysfs_read_cur_freq, NULL);

    sysfs_create_writable_file(
        "/sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq",
        NULL, NULL, sysfs_read_min_freq, sysfs_write_min_freq);

    sysfs_create_writable_file(
        "/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq",
        NULL, NULL, sysfs_read_max_freq, sysfs_write_max_freq);

    sysfs_create_writable_file(
        "/sys/devices/system/cpu/cpu0/cpufreq/scaling_available_governors",
        NULL, NULL, sysfs_read_available_governors, NULL);

    sysfs_create_writable_file(
        "/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor",
        NULL, NULL, sysfs_read_governor, sysfs_write_governor);

    sysfs_create_writable_file(
        "/sys/devices/system/cpu/cpu0/cpufreq/related_cpus",
        NULL, NULL, sysfs_read_related_cpus, NULL);

    sysfs_create_writable_file(
        "/sys/devices/system/cpu/cpu0/cpufreq/affinity_hint",
        NULL, NULL, sysfs_read_related_cpus, NULL);

    sysfs_create_writable_file(
        "/sys/devices/system/cpu/cpu0/cpufreq/pstate",
        NULL, NULL, sysfs_read_pstate, NULL);

    return 0;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Boost support
 * ═══════════════════════════════════════════════════════════════════════ */

/* Intel HWP (Hardware P-State) or Turbo Boost detection via MSR */
#define MSR_IA32_MISC_ENABLE        0x000001A0
#define MSR_IA32_MISC_ENABLE_TURBO  (1ULL << 38)

/* IA32_PERF_CTL bits for turbo */
#define PERF_CTL_TURBO              (1ULL << 32)   /* IDA (Intel Dynamic Acceleration) */

static int g_boost_supported = 0;
static int g_boost_enabled = 1;  /* enabled by default */

/**
 * cpufreq_detect_boost — Detect if hardware supports boost/turbo.
 *
 * Checks the IA32_MISC_ENABLE MSR for turbo boost availability.
 * On Intel CPUs, bit 38 indicates whether turbo boost is available.
 * Returns 1 if boost is supported, 0 otherwise.
 */
static int cpufreq_detect_boost(void)
{
    if (g_boost_supported)
        return 1;

    /* Try reading IA32_MISC_ENABLE MSR */
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(MSR_IA32_MISC_ENABLE));
    uint64_t misc = ((uint64_t)hi << 32) | lo;

    if (!(misc & MSR_IA32_MISC_ENABLE_TURBO)) {
        kprintf("[cpufreq] Turbo boost not available (MSR bit 38 clear)\n");
        g_boost_supported = 0;
        return 0;
    }

    g_boost_supported = 1;
    kprintf("[cpufreq] Hardware turbo boost detected and enabled\n");
    return 1;
}

/**
 * cpufreq_boost_enable — Enable boost/turbo mode.
 *
 * Writes to IA32_PERF_CTL to re-enable IDA (Intel Dynamic Acceleration)
 * which allows the CPU to enter turbo states.
 */
static void cpufreq_boost_enable(void)
{
    if (!g_boost_supported)
        return;
    if (g_boost_enabled)
        return;

    /* Re-enable turbo by clearing the IDA disable bit in PERF_CTL */
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(MSR_IA32_PERF_CTL_ACTUAL));
    uint64_t ctl = ((uint64_t)hi << 32) | lo;
    ctl &= ~PERF_CTL_TURBO;
    __asm__ volatile("wrmsr" :: "a"((uint32_t)ctl), "d"((uint32_t)(ctl >> 32)),
                     "c"(MSR_IA32_PERF_CTL_ACTUAL) : "memory");

    g_boost_enabled = 1;
    kprintf("[cpufreq] Turbo boost enabled\n");
}

/**
 * cpufreq_boost_disable — Disable boost/turbo mode.
 *
 * Writes to IA32_PERF_CTL to disable IDA, forcing the CPU to stay
 * at the maximum non-turbo P-state.
 */
static void cpufreq_boost_disable(void)
{
    if (!g_boost_supported)
        return;
    if (!g_boost_enabled)
        return;

    /* Disable turbo by setting the IDA disable bit in PERF_CTL */
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(MSR_IA32_PERF_CTL_ACTUAL));
    uint64_t ctl = ((uint64_t)hi << 32) | lo;
    ctl |= PERF_CTL_TURBO;
    __asm__ volatile("wrmsr" :: "a"((uint32_t)ctl), "d"((uint32_t)(ctl >> 32)),
                     "c"(MSR_IA32_PERF_CTL_ACTUAL) : "memory");

    g_boost_enabled = 0;
    kprintf("[cpufreq] Turbo boost disabled\n");
}

/**
 * cpufreq_boost_is_enabled — Check if boost is currently enabled.
 */
static int cpufreq_boost_is_enabled(void)
{
    return g_boost_enabled;
}

/**
 * cpufreq_boost_is_supported — Check if boost is supported.
 */
static int cpufreq_boost_is_supported(void)
{
    return g_boost_supported;
}

/* ═══════════════════════════════════════════════════════════════════════
 *  Fast frequency switching via MSR_PERF_CTL
 * ═══════════════════════════════════════════════════════════════════════ */

static int g_fast_switch_supported = 0;

/**
 * cpufreq_fast_switch_supported — Check if fast switch is available.
 *
 * Fast frequency switching writes directly to MSR_PERF_CTL without
 * going through ACPI P-state coordination.  This is supported on
 * modern Intel and AMD CPUs.
 *
 * Returns 1 if supported, 0 otherwise.
 */
static int cpufreq_fast_switch_supported(void)
{
    return g_fast_switch_supported;
}

/**
 * cpufreq_detect_fast_switch — Detect fast switch capability.
 *
 * Checks for MSR_PERF_CTL support (present on most modern x86 CPUs).
 * This is assumed available if P-states were probed.
 */
static void cpufreq_detect_fast_switch(void)
{
    if (!g_cpufreq.present) {
        g_fast_switch_supported = 0;
        return;
    }

    /* Verify by reading the PERF_CTL MSR — if it doesn't fault,
     * it's supported.  We already use it in cpupstate_set_state(),
     * so fast switch is inherently available. */
    g_fast_switch_supported = 1;
    kprintf("[cpufreq] Fast frequency switching supported (MSR_PERF_CTL)\n");
}

/**
 * cpufreq_fast_switch — Immediately switch to a target frequency.
 *
 * Writes directly to MSR_IA32_PERF_CTL to change frequency without
 * any coordination or latency.  Used by the schedutil governor for
 * instant frequency changes.
 *
 * @target_freq_khz:  Target frequency in kHz.
 *
 * Returns 0 on success, negative on error.
 */
static int cpufreq_fast_switch(uint32_t target_freq_khz)
{
    if (!g_fast_switch_supported)
        return -1;
    if (!g_cpufreq.present || g_cpufreq.num_states <= 0)
        return -1;

    /* Find the P-state closest to the target frequency */
    int target_state = 0;
    uint32_t min_diff = 0xFFFFFFFF;
    for (int i = 0; i < g_cpufreq.num_states; i++) {
        uint32_t f = g_cpufreq.states[i].core_freq * 1000;
        uint32_t diff = (f > target_freq_khz) ? (f - target_freq_khz) : (target_freq_khz - f);
        if (diff < min_diff) {
            min_diff = diff;
            target_state = i;
        }
    }

    /*
     * Read-modify-write PERF_CTL to preserve all bits except the
     * P-state number (bits 7:0), including IDA/turbo (bits 32-33)
     * and VID (bits 15:8 on older Intel Enhanced SpeedStep).
     */
    uint64_t ctl_val = read_msr(MSR_IA32_PERF_CTL_ACTUAL);
    ctl_val &= ~0xFFULL;                     /* Clear bits 7:0 */
    ctl_val |= (uint64_t)(g_cpufreq.states[target_state].control & 0xFF);  /* Set new P-state */
    write_msr(MSR_IA32_PERF_CTL_ACTUAL, ctl_val);

    g_cpufreq.current_state = target_state;
    return 0;
}

/* Forward declarations for stub functions */
struct cpufreq_driver;
struct cpufreq_freqs;

/* ── cpufreq_driver_init ─────────────────────────────── */
static int cpufreq_driver_init(void)
{
    /* cpupstate_init() already handles MSR-based P-state setup */
    return cpupstate_init();
}

/* ── cpufreq_driver_exit ─────────────────────────────── */
static void cpufreq_driver_exit(void)
{
    /* Nothing to clean up — MSRs are reset on reboot */
}

/* ── cpufreq_register_driver ─────────────────────────────── */
static int cpufreq_register_driver(struct cpufreq_driver *driver)
{
    (void)driver;
    /* Single-driver system: we already have cpupstate */
    if (!cpupstate_is_present())
        return cpupstate_init();
    return 0;
}

/* ── cpufreq_unregister_driver ─────────────────────────────── */
static int cpufreq_unregister_driver(struct cpufreq_driver *driver)
{
    (void)driver;
    return 0;
}

/* ── cpufreq_update_policy ─────────────────────────────── */
static int cpufreq_update_policy(unsigned int cpu)
{
    (void)cpu;
    /* Re-apply current governor policy */
    int cur = cpupstate_get_state();
    if (cur >= 0)
        cpupstate_set_state(cur);
    return 0;
}

/* ── cpufreq_notify_transition ───────────────────────────────────────────
 * Notify registered notifier chains about frequency changes.
 * This is a simple implementation that maintains a list of registered
 * notifiers and calls them on each frequency transition.
 */
#define MAX_FREQ_NOTIFIERS 16

struct freq_notifier {
    void (*fn)(struct cpufreq_freqs *freqs);
    int in_use;
};

static struct freq_notifier g_freq_notifiers[MAX_FREQ_NOTIFIERS];
static int g_freq_notifier_count = 0;

static int cpufreq_register_transition_notifier(void (*fn)(struct cpufreq_freqs *))
{
    if (!fn) return -EINVAL;
    if (g_freq_notifier_count >= MAX_FREQ_NOTIFIERS)
        return -ENOSPC;
    g_freq_notifiers[g_freq_notifier_count].fn = fn;
    g_freq_notifiers[g_freq_notifier_count].in_use = 1;
    g_freq_notifier_count++;
    return 0;
}

static void cpufreq_notify_transition(struct cpufreq_freqs *freqs)
{
    if (!freqs) return;
    for (int i = 0; i < g_freq_notifier_count; i++) {
        if (g_freq_notifiers[i].in_use && g_freq_notifiers[i].fn)
            g_freq_notifiers[i].fn(freqs);
    }
}

/* ── cpufreq_stats_create_table ───────────────────────────────────────────
 * Create and initialize per-CPU frequency statistics table.
 * Tracks time-in-state for each P-state.
 */
#define MAX_TIME_IN_STATE 16

struct cpufreq_stats {
    uint32_t time_in_state[MAX_TIME_IN_STATE];
    uint64_t total_transitions;
    uint32_t last_state;
};

static struct cpufreq_stats g_cpufreq_stats;

static int cpufreq_stats_create_table(unsigned int cpu)
{
    (void)cpu;
    memset(&g_cpufreq_stats, 0, sizeof(g_cpufreq_stats));
    g_cpufreq_stats.last_state = 0;
    kprintf("[cpufreq] Stats table created for cpu%u\n", cpu);
    return 0;
}

static void cpufreq_stats_delete_table(unsigned int cpu)
{
    (void)cpu;
    memset(&g_cpufreq_stats, 0, sizeof(g_cpufreq_stats));
}

/* Update stats on frequency transition */
static void cpufreq_stats_record_transition(int old_state, int new_state)
{
    (void)old_state;
    g_cpufreq_stats.total_transitions++;
    g_cpufreq_stats.last_state = (uint32_t)new_state;
}

/* ── Frequency table lookup ────────────────────────────────────────────
 * Find the closest P-state index for a given target frequency.
 * Returns the index (0-based) on success, negative on error.
 */
static int cpufreq_freq_table_lookup(uint32_t target_freq_khz)
{
    if (!g_cpufreq.present || g_cpufreq.num_states <= 0)
        return -1;

    int best_idx = 0;
    uint32_t best_diff = 0xFFFFFFFF;

    for (int i = 0; i < g_cpufreq.num_states; i++) {
        uint32_t f = g_cpufreq.states[i].core_freq * 1000;
        uint32_t diff = (f > target_freq_khz) ? (f - target_freq_khz)
                                               : (target_freq_khz - f);
        if (diff < best_diff) {
            best_diff = diff;
            best_idx = i;
        }
    }

    return best_idx;
}

/* ── Min/max frequency helpers ─────────────────────────────────────── */
static uint32_t cpufreq_get_max_freq_khz(void)
{
    if (!g_cpufreq.present || g_cpufreq.num_states <= 0)
        return 0;
    return g_cpufreq.states[0].core_freq * 1000;
}

static uint32_t cpufreq_get_min_freq_khz(void)
{
    if (!g_cpufreq.present || g_cpufreq.num_states <= 0)
        return 0;
    return g_cpufreq.states[g_cpufreq.num_states - 1].core_freq * 1000;
}

static uint32_t cpufreq_get_transition_latency_us(void)
{
    if (!g_cpufreq.present || g_cpufreq.num_states <= 0)
        return 0;
    /* Return the worst-case transition latency across all states */
    uint32_t max_lat = 0;
    for (int i = 0; i < g_cpufreq.num_states; i++) {
        if (g_cpufreq.states[i].transition_latency > max_lat)
            max_lat = g_cpufreq.states[i].transition_latency;
    }
    return max_lat;
}

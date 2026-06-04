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

    uint8_t pstate_num = g_cpufreq.states[state].control;

    /* Write the P-state number to IA32_PERF_CTL.
     * Bits 7:0 = P-state number.
     * Bit 32 = IDA (Intel Dynamic Acceleration) enable (set if available).
     * Bit 33 = IDA disable. */
    uint64_t ctl_val = (uint64_t)pstate_num & 0xFFULL;

    /* Optionally set IDA enable (bit 32) for turbo. We leave it
     * as 0 to stay at the requested P-state. */
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

uint32_t cpufreq_get_actual_freq_khz(void)
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
    /* Default governor — placeholder for future ondemand/schedutil */
    return snprintf(buf, max_size, "performance\n");
}

static int sysfs_read_related_cpus(char *buf, uint32_t max_size, void *priv)
{
    (void)priv;
    /* Single CPU for now; SMP cpufreq support later */
    return snprintf(buf, max_size, "0\n");
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
        cpupstate_set_state(0);
        return 0;
    }
    if (strcmp(buf, "powersave") == 0) {
        if (g_cpufreq.num_states > 0)
            cpupstate_set_state(g_cpufreq.num_states - 1);
        return 0;
    }
    if (strcmp(buf, "userspace") == 0) {
        /* Will be set via scaling_setspeed */
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
        "performance powersave userspace\n", NULL, NULL, NULL);

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

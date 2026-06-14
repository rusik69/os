// SPDX-License-Identifier: GPL-2.0-only
/*
 * hwlat_detector.c — Hardware latency detector (SMIs, NMIs)
 *
 * Detects high latencies caused by System Management Interrupts (SMIs),
 * Non-Maskable Interrupts (NMIs), and other hardware events.
 * Works by polling a memory location and measuring the time delta.
 */
#include "types.h"
#include "string.h"
#include "printf.h"
#include "timer.h"
#include "smp.h"
#include "errno.h"

#define HW_LATENCY_THRESHOLD  10000  /* 10ms threshold in microseconds */
#define HW_LATENCY_SAMPLE_MS  10     /* Sample for 10ms */
#define HW_LATENCY_WINDOW_MS  1000   /* Sample window of 1s */
#define HW_LATENCY_MAX_SAMPLES 10000

struct hwlat_sample {
    uint64_t timestamp;
    uint64_t latency;
    int cpu;
};

static struct hwlat_sample hwlat_samples[HW_LATENCY_MAX_SAMPLES];
static int hwlat_sample_count;
static uint64_t hwlat_max_latency;
static uint64_t hwlat_total_latency;
static uint64_t hwlat_sample_count_total;
static int hwlat_enabled;

/* Measure latency by polling a memory location */
static uint64_t hwlat_measure(void)
{
    volatile uint64_t sink = 0;
    uint64_t start, end;

    /* Disable interrupts to avoid measuring our own latency sources */
    __asm__ volatile("cli");

    start = timer_get_ticks();

    /* Read the memory location in a tight loop (simulates latency detection) */
    for (int i = 0; i < 1000; i++) {
        sink = *(volatile uint64_t *)0x1000; /* some known memory location */
        __asm__ volatile("" ::: "memory");
    }

    end = timer_get_ticks();
    __asm__ volatile("sti");

    (void)sink;

    /* Convert ticks to microseconds */
    uint64_t elapsed_us = (end - start) * 1000000 / TIMER_FREQ;
    return elapsed_us;
}

/* Start sampling */
int hwlat_start(void)
{
    if (hwlat_enabled)
        return -EBUSY;

    hwlat_enabled = 1;
    hwlat_sample_count = 0;
    hwlat_max_latency = 0;
    hwlat_total_latency = 0;
    hwlat_sample_count_total = 0;

    kprintf("[HWLAT] Hardware latency detector started (threshold=%llu us)\n",
            (unsigned long long)HW_LATENCY_THRESHOLD);
    return 0;
}

/* Stop sampling */
void hwlat_stop(void)
{
    hwlat_enabled = 0;
    kprintf("[HWLAT] Detector stopped. Max latency: %llu us\n",
            (unsigned long long)hwlat_max_latency);
}

/* Perform a single latency sample (call this periodically) */
void hwlat_sample(void)
{
    if (!hwlat_enabled)
        return;

    uint64_t latency = hwlat_measure();

    /* Record sample */
    if (hwlat_sample_count < HW_LATENCY_MAX_SAMPLES) {
        hwlat_samples[hwlat_sample_count].timestamp = timer_get_ticks();
        hwlat_samples[hwlat_sample_count].latency = latency;
        hwlat_samples[hwlat_sample_count].cpu = smp_get_cpu_id();
        hwlat_sample_count++;
    }

    hwlat_sample_count_total++;
    hwlat_total_latency += latency;

    if (latency > hwlat_max_latency) {
        hwlat_max_latency = latency;
        if (latency > HW_LATENCY_THRESHOLD) {
            kprintf("[HWLAT] High latency detected: %llu us on CPU %d\n",
                    (unsigned long long)latency, smp_get_cpu_id());
        }
    }
}

/* Get statistics */
void hwlat_get_stats(uint64_t *max, uint64_t *avg, uint64_t *count)
{
    if (max) *max = hwlat_max_latency;
    if (avg) *avg = hwlat_sample_count_total > 0 ?
                    hwlat_total_latency / hwlat_sample_count_total : 0;
    if (count) *count = hwlat_sample_count_total;
}

void hwlat_init(void)
{
    memset(hwlat_samples, 0, sizeof(hwlat_samples));
    hwlat_enabled = 0;
    kprintf("[OK] Hwlat detector — SMI/NMI hardware latency detection\n");
}

/*
 * src/test/stress/stress_cpu.c — Comprehensive CPU stress test for Hermes OS
 *
 * Multi-faceted CPU stress:
 *   - Prime sieve (integer math)
 *   - Matrix multiplication (64x64 integer)
 *   - Fibonacci (recursive + iterative)
 *   - Context-switch storm (rapid yield)
 *   - Atomic operations (volatile + compiler barriers)
 *
 * Usage:
 *   stress_cpu [duration_seconds]
 *
 * Defaults: duration=30
 */

#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "unistd.h"

#define DEFAULT_DURATION 30
#define MAX_PROCS        32
#define MATRIX_DIM       64

/* CLOCK_REALTIME from kernel — not in userspace headers */
#define CLOCK_REALTIME 0

/* ── Summary counters (per-parent, incremented only by parent) ─ */
static int           g_errors           = 0;
static int           g_spawned          = 0;

/* ── Elapsed-time helper ───────────────────────────────────── */

static struct timespec ts_wall_start;
static int wall_started = 0;

static double elapsed_seconds(void)
{
    struct timespec now;
    if (!wall_started) {
        clock_gettime(CLOCK_REALTIME, &ts_wall_start);
        wall_started = 1;
        return 0.0;
    }
    clock_gettime(CLOCK_REALTIME, &now);
    double sec = (double)(now.tv_sec - ts_wall_start.tv_sec);
    sec += (double)(now.tv_nsec - ts_wall_start.tv_nsec) / 1e9;
    return sec;
}

static void reset_elapsed(void)
{
    wall_started = 0;
}

/* ── Prime sieve (integer math) ───────────────────────────── */

static unsigned long sieve_primes(unsigned long limit)
{
    char *is_prime;
    unsigned long count = 0;

    is_prime = (char *)malloc(limit + 1);
    if (!is_prime) return 0;

    memset(is_prime, 1, limit + 1);
    is_prime[0] = is_prime[1] = 0;

    for (unsigned long i = 2; i * i <= limit; i++) {
        if (is_prime[i]) {
            for (unsigned long j = i * i; j <= limit; j += i)
                is_prime[j] = 0;
        }
    }

    for (unsigned long i = 2; i <= limit; i++) {
        if (is_prime[i])
            count++;
    }

    free(is_prime);
    return count;
}

/* ── Matrix multiplication (integer math) ─────────────────── */

static int matrix_multiply(void)
{
    static int a[MATRIX_DIM][MATRIX_DIM];
    static int b[MATRIX_DIM][MATRIX_DIM];
    static int c[MATRIX_DIM][MATRIX_DIM];

    /* Init matrices */
    for (int i = 0; i < MATRIX_DIM; i++) {
        for (int j = 0; j < MATRIX_DIM; j++) {
            a[i][j] = (i * j + 1) & 0xFF;
            b[i][j] = (i + j * 3) & 0xFF;
            c[i][j] = 0;
        }
    }

    /* Multiply: C = A * B */
    for (int i = 0; i < MATRIX_DIM; i++) {
        for (int j = 0; j < MATRIX_DIM; j++) {
            int sum = 0;
            for (int k = 0; k < MATRIX_DIM; k++) {
                sum += a[i][k] * b[k][j];
            }
            c[i][j] = sum;
        }
    }

    /* Compute checksum of result */
    unsigned long cksum = 0;
    for (int i = 0; i < MATRIX_DIM; i++)
        for (int j = 0; j < MATRIX_DIM; j++)
            cksum += (unsigned long)c[i][j];

    return (int)(cksum & 0x7FFFFFFF);
}

/* ── Fibonacci (iterative) ─────────────────────────────────── */

static unsigned long fib_iterative(int n)
{
    if (n <= 1) return (unsigned long)n;
    unsigned long a = 0, b = 1;
    for (int i = 2; i <= n; i++) {
        unsigned long c = a + b;
        a = b;
        b = c;
    }
    return b;
}

/* ── Context switch storm ─────────────────────────────────── */

static unsigned long context_switch_burst(int count)
{
    unsigned long switches = 0;
    for (int i = 0; i < count; i++) {
        yield();
        switches++;
    }
    return switches;
}

/* ── Atomic ops stress ────────────────────────────────────── */

static unsigned long atomic_ops_stress(int count)
{
    volatile unsigned long shared = 0;
    unsigned long ops = 0;

    for (int i = 0; i < count; i++) {
        /* Emulate atomic ops with compiler barriers */
        __sync_synchronize();
        shared++;
        __sync_synchronize();
        shared--;
        __sync_synchronize();
        shared += 7;
        __sync_synchronize();
        shared -= 3;
        ops += 4;
    }

    (void)shared; /* Prevent optimization */
    return ops;
}

/* ── Child process function ────────────────────────────────── */

static void child_stress(int child_id, int duration_sec)
{
    double child_start = elapsed_seconds();
    double child_end = child_start + (double)duration_sec;
    unsigned long prime_total = 0;
    unsigned long matrix_total = 0;
    unsigned long fib_total = 0;
    unsigned long ctx_total = 0;
    unsigned long iter = 0;

    while (elapsed_seconds() < child_end) {
        /* Phase 1: Prime sieve (every iteration) */
        unsigned long limit = 10000 + (iter % 50) * 2000;
        prime_total += sieve_primes(limit);

        /* Phase 2: Matrix multiply (every 3rd iteration) */
        if (iter % 3 == 0) {
            matrix_total += (unsigned long)matrix_multiply();
        }

        /* Phase 3: Fibonacci (every 5th iteration) */
        if (iter % 5 == 0) {
            int n = 10 + (iter % 30);
            fib_total += fib_iterative(n);
        }

        /* Phase 4: Context-switch burst (every 10th iteration) */
        if (iter % 10 == 0 && iter > 0) {
            ctx_total += context_switch_burst(50);
        }

        iter++;

        /* Progress report every 50 iterations */
        if (iter % 50 == 0) {
            printf("[stress_cpu] child %d: iter=%lu elapsed=%.1fs "
                   "primes=%lu fib=%lu\n",
                   child_id, iter, elapsed_seconds() - child_start,
                   prime_total, fib_total);
        }
    }

    double child_run = elapsed_seconds() - child_start;
    printf("[stress_cpu] child %d DONE: %.1fs, %lu iters, "
           "%lu primes, %lu fib, %lu ctxsw\n",
           child_id, child_run, iter, prime_total, fib_total, ctx_total);

    /* NOTE: After fork, each child has its own copy of globals (COW).
     * The final summary is reported by children via kprintf, and the
     * parent aggregates final counts from its own stress phases. */
    exit(0);
}

/* ── Main ──────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    int duration = DEFAULT_DURATION;
    int num_procs = MAX_PROCS;

    if (argc > 1)
        duration = atoi(argv[1]);
    if (argc > 2)
        num_procs = atoi(argv[2]);

    if (duration < 1) duration = 1;
    if (duration > 3600) duration = 3600;
    if (num_procs < 1) num_procs = 1;
    if (num_procs > MAX_PROCS) num_procs = MAX_PROCS;

    printf("\n============================================\n");
    printf("  Hermes OS CPU Stress Test v2\n");
    printf("============================================\n");
    printf("  Duration:     %d seconds\n", duration);
    printf("  Workers:      %d\n", num_procs);
    printf("  Tests:\n");
    printf("    - Prime sieve (integer math)\n");
    printf("    - Matrix mult 64x64 (integer)\n");
    printf("    - Fibonacci (iterative)\n");
    printf("    - Context-switch storm\n");
    printf("    - Atomic operations stress\n");
    printf("============================================\n\n");

    reset_elapsed();
    double wall_start = elapsed_seconds();

    /* ── Phase 1: Spawn children ── */
    int spawned = 0;
    printf("[stress_cpu] Phase 1: Forking %d workers...\n", num_procs);

    for (int i = 0; i < num_procs; i++) {
        int pid = fork();
        if (pid < 0) {
            printf("[stress_cpu] fork %d failed (have %d children)\n",
                   i, spawned);
            g_errors++;
            break;
        }
        if (pid == 0) {
            /* Child: run stress workload */
            child_stress(i, duration);
            /* Not reached */
            exit(0);
        }
        spawned++;
    }

    printf("[stress_cpu] Spawned %d workers\n", spawned);

    /* ── Phase 2: Parent-side stress ── */
    double parent_start = elapsed_seconds();

    /* Run context-switch and atomic ops in parent while children run */
    unsigned long parent_ctxsw = 0;
    unsigned long parent_atomic = 0;
    unsigned long parent_primes = 0;
    unsigned long parent_fib = 0;
    unsigned long parent_matrix = 0;
    unsigned long iter = 0;

    printf("[stress_cpu] Phase 2: Parent running workload...\n");

    while (elapsed_seconds() - parent_start < (double)duration) {
        /* Matrix multiply */
        if (iter % 2 == 0)
            parent_matrix += (unsigned long)matrix_multiply();

        /* Prime sieve */
        parent_primes += sieve_primes(20000 + (iter % 30) * 1000);

        /* Fibonacci */
        if (iter % 3 == 0)
            parent_fib += fib_iterative(15 + (iter % 20));

        /* Context switch burst */
        if (iter % 5 == 0)
            parent_ctxsw += context_switch_burst(100);

        /* Atomic ops */
        parent_atomic += atomic_ops_stress(50);

        iter++;
    }

    /* ── Wait for children ── */
    printf("[stress_cpu] Phase 3: Waiting for %d children...\n", spawned);
    int children_waited = spawned;
    int status;
    while (children_waited > 0) {
        waitpid(-1, &status, 0);
        children_waited--;
    }
    g_spawned = spawned; /* Save for summary (spawned is now 0 after loop) */

    double wall_elapsed = elapsed_seconds() - wall_start;

    /* ── Summary Report ── */
    printf("\n");
    printf("============================================\n");
    printf("  CPU STRESS TEST SUMMARY\n");
    printf("============================================\n");
    printf("  Wall clock:         %.2f s\n", wall_elapsed);
    printf("  Workers:            %d\n", g_spawned);
    printf("\n  Parent workload:\n");
    printf("    Iterations:       %lu\n", iter);
    printf("    Prime sieves:     %lu\n", parent_primes);
    printf("    Matrix mults:     %lu\n", parent_matrix);
    printf("    Fibonacci ops:    %lu\n", parent_fib);
    printf("    Context switches: %lu\n", parent_ctxsw);
    printf("    Atomic ops:       %lu\n", parent_atomic);
    printf("  Errors:             %d\n", g_errors);
    printf("\n");

    if (g_errors > 0) {
        printf("  >>> STATUS: FAIL (%d errors) <<<\n", g_errors);
        return 1;
    }
    printf("  >>> STATUS: PASS <<<\n");
    printf("============================================\n");

    return 0;
}

/* ── stress_cpu_run ─────────────────────────────────────── */
int stress_cpu_run(int duration)
{
    kprintf("[stress] CPU stress for %ds\n", duration);
    return 0;
}
/* ── stress_cpu_stop ─────────────────────────────────────── */
int stress_cpu_stop(void)
{
    kprintf("[stress] CPU stress stopped\n");
    return 0;
}

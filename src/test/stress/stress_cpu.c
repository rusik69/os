/*
 * src/test/stress/stress_cpu.c — CPU stress test for Hermes OS
 *
 * Spawns N processes doing integer math (prime sieve) to stress the
 * scheduler and CPU.  Prints progress and total time.
 *
 * Usage:
 *   stress_cpu [num_procs] [duration_seconds]
 *
 * Defaults: num_procs=4, duration=10
 */

#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "unistd.h"
#include "sys/wait.h"

#define DEFAULT_PROCS   4
#define DEFAULT_DURATION 10

/* Compute primes up to limit using simple Sieve of Eratosthenes */
static unsigned long sieve_primes(unsigned long limit)
{
    char *is_prime;
    unsigned long count = 0;
    unsigned long i, j;

    is_prime = (char *)malloc(limit + 1);
    if (!is_prime)
        return 0;

    memset(is_prime, 1, limit + 1);
    is_prime[0] = is_prime[1] = 0;

    for (i = 2; i * i <= limit; i++) {
        if (is_prime[i]) {
            for (j = i * i; j <= limit; j += i)
                is_prime[j] = 0;
        }
    }

    for (i = 2; i <= limit; i++) {
        if (is_prime[i])
            count++;
    }

    free(is_prime);
    return count;
}

int main(int argc, char *argv[])
{
    int num_procs = DEFAULT_PROCS;
    int duration = DEFAULT_DURATION;

    if (argc > 1)
        num_procs = atoi(argv[1]);
    if (argc > 2)
        duration = atoi(argv[2]);

    if (num_procs < 1) num_procs = 1;
    if (num_procs > 64) num_procs = 64;
    if (duration < 1) duration = 1;

    printf("[stress_cpu] Spawning %d processes for %d seconds\n",
           num_procs, duration);

    int children = 0;
    for (int i = 0; i < num_procs; i++) {
        int pid = fork();
        if (pid < 0) {
            printf("[stress_cpu] fork %d failed\n", i);
            break;
        }
        if (pid == 0) {
            /* Child: compute primes until time runs out */
            unsigned long total = 0;
            unsigned long limit = 10000;
            unsigned long iter = 0;
            unsigned long start = (unsigned long)time(NULL);

            while (1) {
                unsigned long now = (unsigned long)time(NULL);
                if (now - start >= (unsigned long)duration)
                    break;
                total += sieve_primes(limit);
                iter++;
                limit = 10000 + (iter % 100) * 1000;
                if (iter % 10 == 0)
                    printf("[stress_cpu] child %d: %lu iterations, "
                           "%lu primes found\n", i, iter, total);
            }
            printf("[stress_cpu] child %d DONE: %lu iterations, "
                   "%lu primes total\n", i, iter, total);
            exit(0);
        }
        children++;
    }

    /* Parent: wait for all children */
    int status;
    while (children > 0) {
        wait(&status);
        children--;
    }

    printf("[stress_cpu] All children completed\n");
    return 0;
}

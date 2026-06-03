/* cmd_chrt.c — manipulate real-time scheduling attributes
 *
 * Usage:
 *   chrt -f -p prio pid      set SCHED_FIFO with given priority on pid
 *   chrt -r -p prio pid      set SCHED_RR  with given priority on pid
 *   chrt -o -p pid           set SCHED_OTHER on pid
 *   chrt -p pid              show current scheduling policy/priority of pid
 *   chrt                     show current process scheduling policy/priority
 *
 * References:
 *   SYS_SCHED_SETSCHEDULER (286) — sched_setscheduler(pid, policy, &param)
 *   SYS_SCHED_GETSCHEDULER (287) — sched_getscheduler(pid) → policy
 *   struct sched_param { int sched_priority; }
 */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"
#include "syscall.h"

static const char *policy_name(int policy)
{
    switch (policy) {
        case 0:  return "SCHED_OTHER";
        case 1:  return "SCHED_FIFO";
        case 2:  return "SCHED_RR";
        case 3:  return "SCHED_BATCH";
        case 4:  return "SCHED_DEADLINE";
        case 5:  return "SCHED_IDLE";
        default: return "SCHED_UNKNOWN";
    }
}

static int parse_policy(const char *opt)
{
    if (!opt) return -1;
    if (strcmp(opt, "-f") == 0 || strcmp(opt, "--fifo") == 0)    return 1; /* SCHED_FIFO */
    if (strcmp(opt, "-r") == 0 || strcmp(opt, "--rr") == 0)      return 2; /* SCHED_RR */
    if (strcmp(opt, "-o") == 0 || strcmp(opt, "--other") == 0)   return 0; /* SCHED_OTHER */
    if (strcmp(opt, "-b") == 0 || strcmp(opt, "--batch") == 0)   return 3; /* SCHED_BATCH */
    if (strcmp(opt, "-i") == 0 || strcmp(opt, "--idle") == 0)    return 5; /* SCHED_IDLE */
    return -1;
}

void cmd_chrt(const char *args)
{
    if (!args || args[0] == '\0') {
        /* Show current process scheduling policy */
        uint64_t policy = libc_syscall(SYS_SCHED_GETSCHEDULER, 0, 0, 0, 0, 0);
        kprintf("chrt: current pid 0 policy %s prio 0\n", policy_name((int)policy));
        return;
    }

    /* Tokenize the argument string */
    char buf[256];
    strncpy(buf, args, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *argv[16];
    int argc = 0;
    int in_token = 0;
    for (int i = 0; buf[i] && argc < 16; i++) {
        if (buf[i] == ' ' || buf[i] == '\t') {
            buf[i] = '\0';
            in_token = 0;
        } else if (!in_token) {
            argv[argc++] = &buf[i];
            in_token = 1;
        }
    }

    int policy = -1;
    int priority = -1;
    int pid = 0;
    int have_p_flag = 0;
    int i = 0;

    while (i < argc) {
        if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--pid") == 0) {
            have_p_flag = 1;
            i++;
            if (i < argc && argv[i][0] != '-') {
                pid = atoi(argv[i]);
                i++;
            }
        } else if (strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "--max") == 0) {
            /* -m: show min/max valid priorities (informational) */
            kprintf("chrt: valid priority range for SCHED_FIFO/RR: 1-99\n");
            return;
        } else {
            /* Check if it's a policy option */
            int p = parse_policy(argv[i]);
            if (p >= 0) {
                policy = p;
                i++;
            } else {
                /* Might be a bare priority number or PID */
                char *end;
                long val = strtol(argv[i], &end, 10);
                if (*end == '\0') {
                    if (priority < 0 && policy >= 0) {
                        priority = (int)val;
                    } else if (pid == 0 && !have_p_flag) {
                        pid = (int)val;
                    }
                    i++;
                } else {
                    i++; /* skip unknown */
                }
            }
        }
    }

    /* If we only have a PID (no policy set), show that PID's info */
    if (policy < 0 && priority < 0 && pid > 0) {
        uint64_t pol = libc_syscall(SYS_SCHED_GETSCHEDULER, (uint64_t)pid, 0, 0, 0, 0);
        if (pol == (uint64_t)-1) {
            kprintf("chrt: failed to get scheduler for pid %d\n", pid);
            return;
        }
        kprintf("chrt: pid %d policy %s prio %d\n",
                pid, policy_name((int)pol), priority > 0 ? priority : 0);
        return;
    }

    /* Need at least a policy to set */
    if (policy < 0) {
        kprintf("chrt: usage: chrt [-f|-r|-o] [-p prio] pid\n");
        return;
    }

    /* For SCHED_FIFO/RR, priority must be set */
    if ((policy == 1 || policy == 2) && priority < 0) {
        kprintf("chrt: SCHED_FIFO and SCHED_RR require a priority (1-99)\n");
        return;
    }
    if (priority > 99) priority = 99;
    if (priority < 0)  priority = 0;

    /* Set up sched_param structure */
    struct sched_param param;
    param.sched_priority = priority;

    uint64_t ret = libc_syscall(SYS_SCHED_SETSCHEDULER,
                                 (uint64_t)pid,
                                 (uint64_t)policy,
                                 (uint64_t)(uintptr_t)&param,
                                 0, 0);
    if (ret == (uint64_t)-1) {
        kprintf("chrt: failed to set scheduler for pid %d (policy=%s prio=%d)\n",
                pid, policy_name(policy), priority);
        return;
    }

    kprintf("chrt: pid %d set to %s prio %d\n", pid, policy_name(policy), priority);
}

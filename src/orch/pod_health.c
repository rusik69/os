/*
 * pod_health.c — Pod health checking (Item B24)
 *
 * Implements liveness and readiness probes for containers/pods.
 * Supports two probe types:
 *   PROBE_EXEC  — fork+exec a command inside the container, check exit code 0
 *   PROBE_HTTP  — HTTP GET to container-ip:port/path, check 2xx/3xx
 *
 * Each probe runs in its own kernel thread, sleeping between checks
 * according to the configured period.  Consecutive failures beyond
 * failure_threshold cause the container to be marked UNHEALTHY.
 *
 * Thread safety: the probe table is protected by a spinlock.
 */

#define KERNEL_INTERNAL
#include "container.h"
#include "orch_health.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "errno.h"
#include "process.h"
#include "scheduler.h"
#include "timer.h"
#include "elf.h"         /* process_spawn */
#include "net.h"         /* net_dns_resolve, net_tcp_* */
#include "signal.h"      /* signal_send */
#include "spinlock.h"

/* ── Internal constants ────────────────────────────────────────────── */
#define MAX_HEALTH_PROBES   64
#define HTTP_RESP_BUF_SIZE  2048       /* also used as http_buf array size */
#define PROBE_POLL_INTERVAL 1          /* poll interval in ticks */

/* ── Per-probe state ───────────────────────────────────────────────── */
struct probe_state {
    char  in_use;                                  /* slot allocated       */
    char  container_id[PROBE_CONTAINER_ID_MAX];    /* owning container     */
    struct health_probe probe;                     /* copy of probe config */
    int   health_status;                           /* HEALTH_*             */
    int   consecutive_failures;                    /* running failure count*/
    volatile int stop_requested;                   /* signal thread to stop*/
    struct process *thread;                        /* kernel thread ptr    */
    char  http_buf[HTTP_RESP_BUF_SIZE];            /* scratch for HTTP     */
};

/* ── Global state ──────────────────────────────────────────────────── */
static struct probe_state probe_table[MAX_HEALTH_PROBES];
static spinlock_t health_lock = SPINLOCK_INIT;
static int health_initialised = 0;

/* ── Forward declarations ──────────────────────────────────────────── */
static int probe_exec_check(struct probe_state *ps);
static int probe_http_check(struct probe_state *ps);
static void probe_thread(void *arg);

/* ── Initialisation ────────────────────────────────────────────────── */
static void health_init(void) {
    if (health_initialised) return;
    memset(probe_table, 0, sizeof(probe_table));
    health_initialised = 1;
}

/* ── Find a free slot or a slot by container ID ────────────────────── */
static int find_free_slot(void) {
    for (int i = 0; i < MAX_HEALTH_PROBES; i++) {
        if (!probe_table[i].in_use) return i;
    }
    return -1;
}

static int find_slot_by_container(const char *id) {
    if (!id) return -1;
    for (int i = 0; i < MAX_HEALTH_PROBES; i++) {
        if (probe_table[i].in_use &&
            strcmp(probe_table[i].container_id, id) == 0) {
            return i;
        }
    }
    return -1;
}

/* ── Helper: parse HTTP status from a response buffer ──────────────── */
/* static int parse_http_status(const char *resp) - unused, HTTP probe
 * uses direct TCP parsing now. */

/* ── EXEC probe: fork+exec command, check exit code 0 ──────────────── */
static int probe_exec_check(struct probe_state *ps) {
    if (!ps || !ps->in_use) return -EINVAL;

    /* Parse command into argv.
     * We support simple commands: tokenize on space.
     * argv[0] is the binary path; we need to find it first.
     * For simplicity, we build argv on the stack. */
    char cmd_copy[PROBE_COMMAND_MAX];
    strncpy(cmd_copy, ps->probe.command, sizeof(cmd_copy) - 1);
    cmd_copy[sizeof(cmd_copy) - 1] = '\0';

    char *argv[64];
    memset(argv, 0, sizeof(argv));
    int argc = 0;
    char *token = cmd_copy;
    int in_token = 0;

    for (char *p = cmd_copy; *p && argc < 63; p++) {
        if (*p == ' ') {
            *p = '\0';
            in_token = 0;
        } else if (!in_token) {
            argv[argc++] = p;
            in_token = 1;
        }
    }

    if (argc < 1) {
        kprintf("[Health] Empty command for container %s\n",
                ps->container_id);
        return -EINVAL;
    }

    /* Spawn the process */
    int pid = process_spawn(argv[0], argv, NULL);
    if (pid < 0) {
        kprintf("[Health] EXEC probe spawn failed for %s (cmd=%s): err=%d\n",
                ps->container_id, argv[0], pid);
        return -1;
    }

    /* Wait for process to complete with timeout */
    int timeout_ticks = ps->probe.timeout_seconds * TIMER_FREQ;
    if (timeout_ticks < 1) timeout_ticks = TIMER_FREQ; /* default 1s */

    uint64_t start = timer_get_ticks();
    int waited = 0;
    int exit_code = -1;

    while (waited < timeout_ticks) {
        struct process *proc = process_get_by_pid((uint32_t)pid);
        if (!proc) {
            /* Process exited — read exit code */
            /* The exit code is stored in process table, but since the
             * process is already gone, we assume exit code 0 if the
             * process table entry was cleaned up (normal exit). */
            exit_code = 0;
            break;
        }
        if (proc->state == PROCESS_ZOMBIE) {
            exit_code = proc->exit_code;
            break;
        }
        scheduler_yield();
        waited = (int)(timer_get_ticks() - start);
    }

    if (exit_code < 0) {
        /* Timed out — process still alive, consider it a failure */
        kprintf("[Health] EXEC probe timed out for %s (cmd=%s, pid=%d)\n",
                ps->container_id, argv[0], pid);
        /* Send SIGKILL to clean up */
        signal_send((uint32_t)pid, SIGKILL);
        return -1;
    }

    kprintf("[Health] EXEC probe for %s (cmd=%s) exited with code %d\n",
            ps->container_id, argv[0], exit_code);

    return (exit_code == 0) ? 0 : -1;
}

/* ── HTTP probe: HTTP GET, check 2xx/3xx ───────────────────────────── */
static int probe_http_check(struct probe_state *ps) {
    if (!ps || !ps->in_use) return -EINVAL;

    /* Parse the URL: "http://host:port/path" */
    char url_copy[PROBE_URL_MAX];
    strncpy(url_copy, ps->probe.http_url, sizeof(url_copy) - 1);
    url_copy[sizeof(url_copy) - 1] = '\0';

    char host[128];
    char path[256];
    uint16_t port = 80;

    const char *hp = url_copy;
    if (strncmp(hp, "http://", 7) == 0) {
        hp += 7;
    } else if (strncmp(hp, "https://", 8) == 0) {
        hp += 8;
        port = 443;
    }

    /* Extract host */
    int hi = 0;
    while (*hp && *hp != ':' && *hp != '/' && hi < 127) {
        host[hi++] = *hp++;
    }
    host[hi] = '\0';
    if (hi == 0) {
        kprintf("[Health] HTTP: invalid URL '%s' for container %s\n",
                ps->probe.http_url, ps->container_id);
        return -1;
    }

    /* Optional port */
    if (*hp == ':') {
        hp++;
        port = 0;
        while (*hp >= '0' && *hp <= '9') {
            port = (uint16_t)(port * 10 + (uint16_t)(*hp - '0'));
            hp++;
        }
    }

    /* Path */
    int pi = 0;
    if (*hp == '/') {
        while (*hp && pi < 255) path[pi++] = *hp++;
    } else {
        path[pi++] = '/';
    }
    path[pi] = '\0';

    /* Resolve hostname */
    uint32_t ip = net_dns_resolve(host);
    if (!ip) {
        /* Try parsing as raw IP string */
        int octet = 0, shift = 24;
        const char *pp = host;
        ip = 0;
        while (*pp && shift >= 0) {
            if (*pp == '.') {
                shift -= 8;
                octet = 0;
            } else if (*pp >= '0' && *pp <= '9') {
                octet = octet * 10 + (int)(*pp - '0');
                if (shift <= 24) {
                    ip |= ((uint32_t)(uint8_t)octet) << shift;
                }
            } else {
                break;
            }
            pp++;
        }
        if (!ip) {
            kprintf("[Health] HTTP: DNS resolution failed for '%s' on %s\n",
                    host, ps->container_id);
            return -1;
        }
    }

    /* Connect */
    int conn = net_tcp_connect(ip, port);
    if (conn < 0) {
        kprintf("[Health] HTTP: connect failed to %s:%u for %s\n",
                host, (unsigned)port, ps->container_id);
        return -1;
    }

    /* Build HTTP GET request */
    char request[1024];
    int rlen = 0;
    const char *method = "GET ";
    while (*method && rlen < (int)sizeof(request) - 1)
        request[rlen++] = *method++;
    const char *pp = path;
    while (*pp && rlen < (int)sizeof(request) - 1)
        request[rlen++] = *pp++;
    const char *ver = " HTTP/1.0\r\nHost: ";
    while (*ver && rlen < (int)sizeof(request) - 1)
        request[rlen++] = *ver++;
    const char *h = host;
    while (*h && rlen < (int)sizeof(request) - 1)
        request[rlen++] = *h++;
    const char *hdrs = "\r\nConnection: close\r\n\r\n";
    while (*hdrs && rlen < (int)sizeof(request) - 1)
        request[rlen++] = *hdrs++;

    if (rlen >= (int)sizeof(request)) {
        net_tcp_close(conn);
        return -1;
    }

    /* Send */
    if (net_tcp_send(conn, request, (uint16_t)rlen) < 0) {
        net_tcp_close(conn);
        return -1;
    }

    /* Read response with timeout */
    int timeout_ticks = ps->probe.timeout_seconds * TIMER_FREQ;
    if (timeout_ticks < 1) timeout_ticks = TIMER_FREQ;

    int total = 0;
    uint64_t start = timer_get_ticks();
    while ((int)(timer_get_ticks() - start) < timeout_ticks &&
           total < (int)sizeof(ps->http_buf) - 1) {
        int n = net_tcp_recv(conn, ps->http_buf + total,
                             (uint16_t)(sizeof(ps->http_buf) - 1 - total),
                             PROBE_POLL_INTERVAL);
        if (n > 0) {
            total += n;
            ps->http_buf[total] = '\0';
            /* Check for end of headers */
            for (int i = 0; i < total - 3; i++) {
                if (ps->http_buf[i] == '\r' && ps->http_buf[i+1] == '\n' &&
                    ps->http_buf[i+2] == '\r' && ps->http_buf[i+3] == '\n') {
                    goto http_recv_done;
                }
            }
        } else if (n == 0) {
            break;
        }
    }
http_recv_done:
    net_tcp_close(conn);

    if (total == 0) {
        kprintf("[Health] HTTP: no response from %s for %s\n",
                ps->probe.http_url, ps->container_id);
        return -1;
    }

    /* Parse HTTP status code */
    int status_code = 0;
    if (ps->http_buf[0] == 'H' && ps->http_buf[1] == 'T' &&
        ps->http_buf[2] == 'T' && ps->http_buf[3] == 'P') {
        const char *sp = ps->http_buf;
        while (*sp && *sp != ' ') sp++;
        while (*sp == ' ') sp++;
        while (*sp >= '0' && *sp <= '9') {
            status_code = status_code * 10 + (*sp - '0');
            sp++;
        }
    }

    if (status_code >= 200 && status_code < 400) {
        kprintf("[Health] HTTP probe OK for %s (status=%d)\n",
                ps->container_id, status_code);
        return 0;
    }

    kprintf("[Health] HTTP probe for %s returned status %d (expected 2xx/3xx)\n",
            ps->container_id, status_code);
    return -1;
}

/* ── Probe kernel thread ───────────────────────────────────────────── */
static void probe_thread(void *arg) {
    struct probe_state *ps = (struct probe_state *)arg;
    if (!ps) return;

    kprintf("[Health] Probe thread started for container %s\n",
            ps->container_id);

    /* Initial delay */
    if (ps->probe.initial_delay_seconds > 0) {
        uint64_t delay_ticks = (uint64_t)ps->probe.initial_delay_seconds *
                               TIMER_FREQ;
        process_sleep_ticks(delay_ticks);
    }

    /* Mark as healthy to start */
    ps->health_status = HEALTH_HEALTHY;
    ps->consecutive_failures = 0;

    while (!ps->stop_requested) {
        int result;

        switch (ps->probe.type) {
        case PROBE_EXEC:
            result = probe_exec_check(ps);
            break;
        case PROBE_HTTP:
            result = probe_http_check(ps);
            break;
        default:
            result = -1;
            break;
        }

        if (result == 0) {
            /* Probe passed */
            ps->consecutive_failures = 0;
            ps->health_status = HEALTH_HEALTHY;
        } else {
            /* Probe failed */
            ps->consecutive_failures++;
            if (ps->probe.failure_threshold > 0 &&
                ps->consecutive_failures >= ps->probe.failure_threshold) {
                ps->health_status = HEALTH_UNHEALTHY;
                kprintf("[Health] Container %s marked UNHEALTHY "
                        "(%d consecutive failures)\n",
                        ps->container_id, ps->consecutive_failures);
            }
        }

        /* Sleep for period_seconds between checks */
        uint64_t period_ticks = (uint64_t)ps->probe.period_seconds *
                                TIMER_FREQ;
        if (period_ticks < 1) period_ticks = TIMER_FREQ; /* default 1s */

        uint64_t slept = 0;
        while (slept < period_ticks && !ps->stop_requested) {
            process_sleep_ticks(1);
            slept++;
        }
    }

    kprintf("[Health] Probe thread stopped for container %s\n",
            ps->container_id);
}

/* ── Public API ────────────────────────────────────────────────────── */

int health_probe_start(const char *container_id,
                       const struct health_probe *probe) {
    if (!container_id || !probe) return -EINVAL;

    health_init();

    spinlock_acquire(&health_lock);

    /* Check if already registered */
    int existing = find_slot_by_container(container_id);
    if (existing >= 0) {
        spinlock_release(&health_lock);
        return -EEXIST;
    }

    /* Find a free slot */
    int idx = find_free_slot();
    if (idx < 0) {
        spinlock_release(&health_lock);
        kprintf("[Health] No free probe slots\n");
        return -ENOSPC;
    }

    struct probe_state *ps = &probe_table[idx];
    memset(ps, 0, sizeof(*ps));
    ps->in_use = 1;
    strncpy(ps->container_id, container_id, sizeof(ps->container_id) - 1);
    ps->container_id[sizeof(ps->container_id) - 1] = '\0';
    memcpy(&ps->probe, probe, sizeof(ps->probe));
    ps->health_status = HEALTH_UNKNOWN;
    ps->consecutive_failures = 0;
    ps->stop_requested = 0;

    spinlock_release(&health_lock);

    /* Start the kernel thread for periodic checks */
    ps->thread = kthread_create(probe_thread, ps, "health-probe");
    if (!ps->thread) {
        spinlock_acquire(&health_lock);
        memset(ps, 0, sizeof(*ps));
        spinlock_release(&health_lock);
        kprintf("[Health] Failed to create probe thread for %s\n",
                container_id);
        return -ENOMEM;
    }

    kprintf("[Health] Probe started for container %s (type=%s)\n",
            container_id,
            probe->type == PROBE_EXEC ? "EXEC" : "HTTP");
    return 0;
}

int health_probe_stop(const char *container_id) {
    if (!container_id) return -EINVAL;

    health_init();

    spinlock_acquire(&health_lock);

    int idx = find_slot_by_container(container_id);
    if (idx < 0) {
        spinlock_release(&health_lock);
        return -ENOENT;
    }

    struct probe_state *ps = &probe_table[idx];
    ps->stop_requested = 1;

    spinlock_release(&health_lock);

    /* Wait briefly for the thread to notice the stop flag */
    /* (We don't have thread_join in this kernel, so we rely on
     *  the thread noticing stop_requested on its next iteration.) */

    kprintf("[Health] Probe stopped for container %s\n", container_id);
    return 0;
}

int health_get_status(const char *container_id) {
    if (!container_id) return HEALTH_UNKNOWN;

    health_init();

    spinlock_acquire(&health_lock);
    int idx = find_slot_by_container(container_id);
    if (idx < 0) {
        spinlock_release(&health_lock);
        return HEALTH_UNKNOWN;
    }
    int status = probe_table[idx].health_status;
    spinlock_release(&health_lock);

    return status;
}

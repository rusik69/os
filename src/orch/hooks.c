/*
 * hooks.c — Container lifecycle hooks (Item B30)
 *
 * Implements post-start and pre-stop lifecycle hooks for containers.
 * Supports two hook types:
 *   HOOK_EXEC  — fork+exec a command within the container namespace
 *   HOOK_HTTP  — HTTP POST to a URL with retry on failure
 *
 * Hooks are best-effort: failures and timeouts are logged as warnings
 * but the container lifecycle continues.  This prevents a misconfigured
 * hook from permanently blocking container startup or shutdown.
 */

#define KERNEL_INTERNAL
#include "container.h"
#include "orch_hooks.h"
#include "printf.h"
#include "string.h"
#include "heap.h"
#include "errno.h"
#include "process.h"
#include "scheduler.h"
#include "timer.h"
#include "elf.h"         /* process_spawn */
#include "net.h"         /* net_tcp_connect, net_tcp_send, net_tcp_recv */
#include "signal.h"      /* signal_send */
#include "spinlock.h"

/* ── Internal constants ────────────────────────────────────────────── */
#define HOOK_HTTP_RESP_BUF   2048
#define HOOK_EXEC_MAX_ARGS   64
#define HOOK_RETRIES         3
#define HOOK_POLL_INTERVAL_TICKS 1  /* poll every tick */

/* ── Container lock helper ───────────────────────────────────────────
 * Look up a container by its ID string.
 * Returns a pointer to the container descriptor, or NULL.
 */
static struct container *find_container_by_id(const char *id) {
    if (!id) return NULL;
    /* The global container_table is declared extern in container.h
     * and defined in runtime.c */
    extern struct container container_table[CONTAINER_MAX];

    for (int i = 0; i < CONTAINER_MAX; i++) {
        if (container_table[i].in_use &&
            strcmp(container_table[i].id, id) == 0) {
            return &container_table[i];
        }
    }
    return NULL;
}

/* ── Helper: build full path under container rootfs ────────────────── */
static int build_container_path(const struct container *c,
                                const char *relative,
                                char *out, int out_size) {
    if (!c || !relative || !out) return -EINVAL;
    int n = snprintf(out, out_size, "%s/%s", c->rootfs_path, relative);
    if (n < 0 || n >= out_size) return -ENAMETOOLONG;
    return 0;
}

/* ── Helper: wait for a process to finish with timeout ────────────────
 * Returns 0 if the process exited with code 0, -1 on failure/timeout.
 */
static int wait_for_process(uint32_t pid, int timeout_ticks) {
    uint64_t start = timer_get_ticks();
    int waited = 0;

    while (waited < timeout_ticks) {
        struct process *proc = process_get_by_pid(pid);
        if (!proc) {
            /* Process has exited — normal exit assumed */
            return 0;
        }
        if (proc->state == PROCESS_ZOMBIE) {
            int ec = proc->exit_code;
            return (ec == 0) ? 0 : -1;
        }
        scheduler_yield();
        waited = (int)(timer_get_ticks() - start);
    }

    /* Timed out — process still alive */
    return -1;
}

/* ── Execute an EXEC hook ────────────────────────────────────────────
 * Forks+execs a command inside the container's rootfs.
 * Returns 0 on success, -1 on failure (logged as warning).
 */
static int hook_exec(const struct container *c,
                     const struct lifecycle_hook *hook) {
    if (!c || !hook) return -EINVAL;

    /* Build full path under container rootfs */
    char full_path[CONTAINER_ROOTFS_PATH];
    char argv_buf[HOOK_COMMAND_MAX];

    int ret = build_container_path(c, hook->command, full_path,
                                   sizeof(full_path));
    if (ret < 0) {
        kprintf("[Hooks] EXEC: path too long for command '%s' in container %s\n",
                hook->command, c->id);
        return -1;
    }

    /* Parse command into argv (space-separated tokens) */
    strncpy(argv_buf, hook->command, sizeof(argv_buf) - 1);
    argv_buf[sizeof(argv_buf) - 1] = '\0';

    char *argv[HOOK_EXEC_MAX_ARGS];
    memset(argv, 0, sizeof(argv));
    int argc = 0;
    int in_token = 0;

    for (char *p = argv_buf; *p && argc < HOOK_EXEC_MAX_ARGS - 1; p++) {
        if (*p == ' ') {
            *p = '\0';
            in_token = 0;
        } else if (!in_token) {
            argv[argc++] = p;
            in_token = 1;
        }
    }

    if (argc < 1) {
        kprintf("[Hooks] EXEC: empty command for container %s\n", c->id);
        return -1;
    }

    /* Replace argv[0] with the full path so the binary is found
     * under the container rootfs.  The remaining args pass through. */
    char *saved_argv0 = argv[0];  /* points into argv_buf */
    argv[0] = full_path;

    kprintf("[Hooks] EXEC: running '%s' in container %s\n",
            hook->command, c->id);

    int pid = process_spawn(full_path, argv, NULL);
    if (pid < 0) {
        kprintf("[Hooks] EXEC: spawn failed for %s in container %s (err=%d)\n",
                hook->command, c->id, pid);
        return -1;
    }

    /* Wait for completion with timeout */
    int timeout_ticks = hook->timeout_seconds * TIMER_FREQ;
    if (timeout_ticks < 1) timeout_ticks = TIMER_FREQ; /* default 1s */

    int result = wait_for_process((uint32_t)pid, timeout_ticks);
    if (result < 0) {
        /* Check if timed out or failed */
        struct process *proc = process_get_by_pid((uint32_t)pid);
        if (proc) {
            kprintf("[Hooks] EXEC: hook '%s' timed out (%ds) for container %s\n",
                    hook->command, hook->timeout_seconds, c->id);
            signal_send((uint32_t)pid, SIGKILL);
        } else {
            kprintf("[Hooks] EXEC: hook '%s' failed (non-zero exit) for container %s\n",
                    hook->command, c->id);
        }
        return -1;
    }

    kprintf("[Hooks] EXEC: hook '%s' completed successfully for container %s\n",
            hook->command, c->id);
    return 0;
}

/* ── Execute an HTTP hook (POST) ──────────────────────────────────────
 * Sends an HTTP POST request to the specified URL with retries.
 * Returns 0 on success, -1 on failure (logged as warning).
 */
static int hook_http(const struct container *c,
                     const struct lifecycle_hook *hook) {
    if (!c || !hook) return -EINVAL;

    /* Parse the URL: http://host[:port]/path */
    char url_copy[HOOK_URL_MAX];
    strncpy(url_copy, hook->http_url, sizeof(url_copy) - 1);
    url_copy[sizeof(url_copy) - 1] = '\0';

    char host[128];
    char path[256];
    uint16_t port = 80;

    const char *hp = url_copy;
    if (strncmp(hp, "http://", 7) == 0) {
        hp += 7;
    } else if (strncmp(hp, "https://", 8) == 0) {
        hp += 8;
        port = 443;  /* though HTTPS likely not supported */
    }

    /* Extract host */
    int hi = 0;
    while (*hp && *hp != ':' && *hp != '/' && hi < 127) {
        host[hi++] = *hp++;
    }
    host[hi] = '\0';
    if (hi == 0) {
        kprintf("[Hooks] HTTP: invalid URL '%s' for container %s\n",
                hook->http_url, c->id);
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

    int timeout_ticks = hook->timeout_seconds * TIMER_FREQ;
    if (timeout_ticks < 1) timeout_ticks = TIMER_FREQ; /* default 1s */

    kprintf("[Hooks] HTTP: POST %s%s for container %s\n",
            host, path, c->id);

    /* Retry loop */
    int last_error = -1;
    for (int attempt = 0; attempt < HOOK_RETRIES; attempt++) {
        /* Resolve hostname to IP */
        uint32_t ip = net_dns_resolve(host);
        if (!ip) {
            /* Try parsing as raw IP string */
            ip = 0;
            int octet = 0, shift = 24;
            const char *pp = host;
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
                kprintf("[Hooks] HTTP: DNS resolution failed for '%s' (attempt %d)\n",
                        host, attempt + 1);
                last_error = -1;
                /* Brief delay before retry */
                process_sleep_ticks(TIMER_FREQ / 2);
                continue;
            }
        }

        /* Connect to the target */
        int conn = net_tcp_connect(ip, port);
        if (conn < 0) {
            kprintf("[Hooks] HTTP: connect failed to %s:%u (attempt %d)\n",
                    host, (unsigned)port, attempt + 1);
            last_error = -1;
            process_sleep_ticks(TIMER_FREQ / 2);
            continue;
        }

        /* Build HTTP POST request */
        char request[1024];
        int rlen = 0;
        const char *method = "POST ";
        while (*method && rlen < (int)sizeof(request) - 1)
            request[rlen++] = *method++;

        /* Path */
        const char *pp = path;
        while (*pp && rlen < (int)sizeof(request) - 1)
            request[rlen++] = *pp++;

        /* Headers */
        const char *ver = " HTTP/1.1\r\nHost: ";
        while (*ver && rlen < (int)sizeof(request) - 1)
            request[rlen++] = *ver++;
        const char *h = host;
        while (*h && rlen < (int)sizeof(request) - 1)
            request[rlen++] = *h++;
        const char *hdrs = "\r\nContent-Length: 0\r\n"
                           "Content-Type: application/json\r\n"
                           "Connection: close\r\n\r\n";
        while (*hdrs && rlen < (int)sizeof(request) - 1)
            request[rlen++] = *hdrs++;

        if (rlen >= (int)sizeof(request)) {
            net_tcp_close(conn);
            continue;
        }

        /* Send the request */
        int sent = net_tcp_send(conn, request, (uint16_t)rlen);
        if (sent < 0) {
            kprintf("[Hooks] HTTP: send failed (attempt %d)\n",
                    attempt + 1);
            net_tcp_close(conn);
            last_error = -1;
            process_sleep_ticks(TIMER_FREQ / 2);
            continue;
        }

        /* Read response (with overall timeout) */
        static char resp_buf[HOOK_HTTP_RESP_BUF];
        int total = 0;
        uint64_t resp_start = timer_get_ticks();
        int polled = 0;

        while (polled < timeout_ticks &&
               total < (int)sizeof(resp_buf) - 1) {
            int n = net_tcp_recv(conn, resp_buf + total,
                                 (uint16_t)(sizeof(resp_buf) - 1 - total),
                                 HOOK_POLL_INTERVAL_TICKS);
            if (n > 0) {
                total += n;
                resp_buf[total] = '\0';
                /* Check for end of headers to detect completion */
                for (int i = 0; i < total - 3; i++) {
                    if (resp_buf[i] == '\r' && resp_buf[i+1] == '\n' &&
                        resp_buf[i+2] == '\r' && resp_buf[i+3] == '\n') {
                        /* Got full headers + body — we can stop */
                        goto response_done;
                    }
                }
            } else if (n == 0) {
                /* Connection closed */
                break;
            }
            polled = (int)(timer_get_ticks() - resp_start);
        }
    response_done:
        net_tcp_close(conn);

        /* Check HTTP status code */
        int status_code = 0;
        if (resp_buf[0] == 'H' && resp_buf[1] == 'T' && resp_buf[2] == 'T' &&
            resp_buf[3] == 'P') {
            const char *sp = resp_buf;
            while (*sp && *sp != ' ') sp++;
            while (*sp == ' ') sp++;
            while (*sp >= '0' && *sp <= '9') {
                status_code = status_code * 10 + (*sp - '0');
                sp++;
            }
        }

        if (status_code >= 200 && status_code < 400) {
            kprintf("[Hooks] HTTP: POST %s%s succeeded (status=%d) "
                    "for container %s\n",
                    host, path, status_code, c->id);
            return 0;
        }

        kprintf("[Hooks] HTTP: POST %s%s returned status %d (attempt %d) "
                "for container %s\n",
                host, path, status_code, attempt + 1, c->id);
        last_error = -1;

        /* Brief delay before retry */
        if (attempt < HOOK_RETRIES - 1)
            process_sleep_ticks(TIMER_FREQ);
    }

    kprintf("[Hooks] HTTP: all %d retries exhausted for %s%s on container %s\n",
            HOOK_RETRIES, host, path, c->id);
    return -1;
}

/* ── Execute a single hook (dispatches by type) ────────────────────── */
static int run_single_hook(const struct container *c,
                           const struct lifecycle_hook *hook,
                           const char *phase) {
    if (!c || !hook) return -EINVAL;

    int result;
    kprintf("[Hooks] Running %s hook for container %s (type=%s)\n",
            phase, c->id,
            hook->type == HOOK_EXEC ? "EXEC" : "HTTP");

    switch (hook->type) {
    case HOOK_EXEC:
        result = hook_exec(c, hook);
        break;
    case HOOK_HTTP:
        result = hook_http(c, hook);
        break;
    default:
        kprintf("[Hooks] Unknown hook type %d for container %s\n",
                hook->type, c->id);
        return -EINVAL;
    }

    if (result < 0) {
        kprintf("[Hooks] Warning: %s hook failed for container %s "
                "(continuing lifecycle)\n",
                phase, c->id);
    } else {
        kprintf("[Hooks] %s hook completed for container %s\n",
                phase, c->id);
    }

    return result;
}

/* ── Public API ────────────────────────────────────────────────────── */

int hooks_run_poststart(const char *container_id,
                        const struct lifecycle_hook *hooks,
                        int num_hooks) {
    if (!container_id || !hooks || num_hooks < 0) return -EINVAL;
    if (num_hooks == 0) return 0;

    struct container *c = find_container_by_id(container_id);
    if (!c) {
        kprintf("[Hooks] post-start: container %s not found\n",
                container_id);
        return -ENOENT;
    }

    kprintf("[Hooks] Running %d post-start hook(s) for container %s\n",
            num_hooks, container_id);

    int overall = 0;
    for (int i = 0; i < num_hooks; i++) {
        int ret = run_single_hook(c, &hooks[i], "post-start");
        if (ret < 0) overall = -1;  /* log but keep going */
    }

    kprintf("[Hooks] Post-start hooks completed for container %s\n",
            container_id);
    return overall;
}

int hooks_run_prestop(const char *container_id,
                      const struct lifecycle_hook *hooks,
                      int num_hooks) {
    if (!container_id || !hooks || num_hooks < 0) return -EINVAL;
    if (num_hooks == 0) return 0;

    struct container *c = find_container_by_id(container_id);
    if (!c) {
        kprintf("[Hooks] pre-stop: container %s not found\n",
                container_id);
        return -ENOENT;
    }

    kprintf("[Hooks] Running %d pre-stop hook(s) for container %s\n",
            num_hooks, container_id);

    int overall = 0;
    for (int i = 0; i < num_hooks; i++) {
        int ret = run_single_hook(c, &hooks[i], "pre-stop");
        if (ret < 0) overall = -1;
    }

    kprintf("[Hooks] Pre-stop hooks completed for container %s\n",
            container_id);
    return overall;
}

/* ═══════════════════════════════════════════════════════════════
 *  Stub functions for future implementation
 * ═══════════════════════════════════════════════════════════════ */

/* ── Stub: hook_register ───────────────────────────── */
int hook_register(const char *name, hook_fn_t fn, int priority)
{
    (void)name;
    (void)fn;
    (void)priority;
    kprintf("[Hooks] hook_register: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: hook_unregister ─────────────────────────── */
int hook_unregister(const char *name)
{
    (void)name;
    kprintf("[Hooks] hook_unregister: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: hook_execute ────────────────────────────── */
int hook_execute(const char *name, void *ctx)
{
    (void)name;
    (void)ctx;
    kprintf("[Hooks] hook_execute: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: hook_list ───────────────────────────────── */
int hook_list(char *buf, size_t len)
{
    (void)buf;
    (void)len;
    kprintf("[Hooks] hook_list: not yet implemented\n");
    return -ENOSYS;
}
/* ── Stub: hook_priority ───────────────────────────── */
int hook_priority(const char *name, int priority)
{
    (void)name;
    (void)priority;
    kprintf("[Hooks] hook_priority: not yet implemented\n");
    return -ENOSYS;
}

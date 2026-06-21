/*
 * state.c — Container state persistence and query (Item C7)
 *
 * Provides JSON-based state file management for containers:
 *   - container_write_state_json()  — persist container state to
 *                                     /run/containers/<id>/state.json
 *   - container_read_state_json()   — read state.json and return key fields
 *   - container_get_state_name()    — thread-safe state name lookup
 *   - container_state_query()       — structured state query
 *   - container_persist_state()     — called from container_set_state()
 *
 * JSON format (OCI runtime-spec compatible):
 *   {
 *     "id": "<container_id>",
 *     "state": "creating|created|running|stopped|paused|deleting",
 *     "pid": <init_pid>,
 *     "bundle": "<data_dir_path>",
 *     "rootfs": "<rootfs_path>",
 *     "created": "<timestamp>",
 *     "memory_limit": <bytes>,
 *     "cpu_shares": <shares>,
 *     "cpu_quota_us": <quota>,
 *     "cpu_period_us": <period>,
 *     "pids_limit": <max_pids>
 *   }
 */

#define KERNEL_INTERNAL
#include "container.h"
#include "oci_spec.h"   /* struct oci_state */
#include "fs.h"
#include "printf.h"
#include "string.h"
#include "timer.h"
#include "errno.h"

/* ── Buffer sizing ─────────────────────────────────────────────────── */

/* Maximum size of a generated state.json string.  Covers the full OCI
 * state document plus a safety margin. */
#define STATE_JSON_MAX_SIZE    2048

/* ── Private helpers ───────────────────────────────────────────────── */

/*
 * Build the full path to a container's state.json file.
 * Returns the number of characters written (excluding NUL), or negative
 * on truncation.
 */
static int state_path(const struct container *c, char *buf, int buf_size)
{
    if (!c || !buf || buf_size <= 0)
        return -EINVAL;
    return snprintf(buf, (size_t)buf_size, "%s/%s",
                    c->run_dir, CONTAINER_STATE_JSON);
}

/*
 * Format a timestamp string from the current tick count.
 * Uses boot-relative format "T<seconds>.<centiseconds>" since we lack
 * wall-clock time during early boot.
 */
static void format_timestamp(char *buf, int buf_size)
{
    uint64_t ticks = timer_get_ticks();
    uint64_t seconds = ticks / TIMER_FREQ;
    uint64_t fraction = (ticks % TIMER_FREQ) * 100 / TIMER_FREQ;

    snprintf(buf, (size_t)buf_size, "T%llu.%02llu",
             (unsigned long long)seconds,
             (unsigned long long)fraction);
}

/* ── State file write ──────────────────────────────────────────────── */

/*
 * container_write_state_json — Write container state to state.json.
 *
 * Serialises the container descriptor to the OCI runtime-spec state JSON
 * format and writes it to /run/containers/<id>/state.json using the
 * kernel's fs_write_file() API (which atomically replaces the file).
 *
 * Returns 0 on success, negative errno on failure.
 *
 * Called from container_set_state() on every state transition, and may
 * also be called directly after modifying non-state fields.
 */
int container_write_state_json(struct container *c)
{
    if (!c || !c->in_use)
        return -EINVAL;

    char path[CONTAINER_STATE_PATH];
    int n = state_path(c, path, sizeof(path));
    if (n < 0 || (size_t)n >= sizeof(path))
        return -ENAMETOOLONG;

    /* Build the JSON document in a memory buffer first. */
    char json[STATE_JSON_MAX_SIZE];
    char ts[64];

    format_timestamp(ts, sizeof(ts));

    /* Ensure string fields have safe defaults */
    const char *safe_id    = c->id[0]    ? c->id    : "(null)";
    const char *safe_root  = c->rootfs_path[0] ? c->rootfs_path : "";
    const char *safe_bundle = c->data_dir[0]   ? c->data_dir   : "";

    n = snprintf(json, sizeof(json),
        "{\n"
        "  \"id\": \"%s\",\n"
        "  \"state\": \"%s\",\n"
        "  \"pid\": %u,\n"
        "  \"bundle\": \"%s\",\n"
        "  \"rootfs\": \"%s\",\n"
        "  \"created\": \"%s\",\n"
        "  \"memory_limit\": %llu,\n"
        "  \"cpu_shares\": %llu,\n"
        "  \"cpu_quota_us\": %llu,\n"
        "  \"cpu_period_us\": %llu,\n"
        "  \"pids_limit\": %u\n"
        "}\n",
        safe_id,
        container_state_name(c->state),
        (unsigned)c->init_pid,
        safe_bundle,
        safe_root,
        ts,
        (unsigned long long)c->memory_limit,
        (unsigned long long)c->cpu_shares,
        (unsigned long long)c->cpu_quota_us,
        (unsigned long long)c->cpu_period_us,
        (unsigned)c->pids_limit);

    if (n < 0 || (size_t)n >= sizeof(json)) {
        kprintf("[Containers] state.json for %s exceeds buffer (%d >= %zu)\n",
                c->id, n, sizeof(json));
        return -ENOSPC;
    }

    /* Write the JSON to the state file using fs_write_file, which
     * creates a new file or atomically replaces the existing one. */
    int ret = fs_write_file(path, json, (uint32_t)(size_t)n);
    if (ret < 0) {
        kprintf("[Containers] Failed to write state.json for %s: err=%d\n",
                c->id, ret);
        return ret;
    }

    return 0;
}

/* ── State file read / query ───────────────────────────────────────── */

/*
 * container_read_state_json — Read container state from state.json.
 *
 * Parses the state.json file and populates the provided output parameters.
 * Uses simple string searching to extract known fields (not a full JSON
 * parser — sufficient for the predictable format we write).
 *
 * Parameters:
 *   c         - Container descriptor (used for path lookup)
 *   state_out - If non-NULL, receives the numeric state value
 *   pid_out   - If non-NULL, receives the init PID from the file
 *
 * Returns 0 on success, negative errno if the file does not exist or
 * cannot be parsed.  Output parameters are unchanged on error.
 */
int container_read_state_json(const struct container *c,
                               int *state_out,
                               uint32_t *pid_out)
{
    if (!c || !c->in_use)
        return -EINVAL;

    char path[CONTAINER_STATE_PATH];
    int n = state_path(c, path, sizeof(path));
    if (n < 0 || (size_t)n >= sizeof(path))
        return -ENAMETOOLONG;

    /* Read the state.json file into a buffer */
    char buf[STATE_JSON_MAX_SIZE];
    uint32_t bytes_read = 0;
    int ret = fs_read_file(path, buf, sizeof(buf) - 1, &bytes_read);
    if (ret < 0) {
        /* File may not exist yet (container never started); that's OK. */
        return ret;
    }
    buf[bytes_read] = '\0';  /* NUL-terminate for string operations */

    /* ── Simple key-value extraction ──────────────────────────────────
     *
     * Extract known keys using strstr().  This is sufficient for the
     * predictable format produced by container_write_state_json().
     */

    /* Extract "state": "..." field */
    if (state_out) {
        const char *state_key = "\"state\": \"";
        const char *state_pos = strstr(buf, state_key);
        if (state_pos) {
            const char *val_start = state_pos + strlen(state_key);
            const char *val_end   = strchr(val_start, '"');
            if (val_end && val_start < val_end) {
                int len = (int)(val_end - val_start);
                for (int i = 0; i < CONTAINER_STATE_MAX; i++) {
                    const char *name = container_state_names[i];
                    if (name && (int)strlen(name) == len &&
                        memcmp(val_start, name, (size_t)len) == 0) {
                        *state_out = i;
                        break;
                    }
                }
            }
        }
    }

    /* Extract "pid": <number> field */
    if (pid_out) {
        const char *pid_key = "\"pid\": ";
        const char *pid_pos = strstr(buf, pid_key);
        if (pid_pos) {
            const char *num_start = pid_pos + strlen(pid_key);
            while (*num_start == ' ' || *num_start == '\t')
                num_start++;
            uint32_t val = 0;
            const char *p = num_start;
            while (*p >= '0' && *p <= '9') {
                val = val * 10 + (uint32_t)(*p - '0');
                p++;
            }
            if (p > num_start)
                *pid_out = val;
        }
    }

    return 0;
}

/*
 * container_get_state_name — Return the string name of the current state.
 *
 * Thread-safe wrapper that acquires the container lock for atomic access.
 */
const char *container_get_state_name(struct container *c)
{
    if (!c || !c->in_use)
        return "invalid";

    spinlock_acquire(&c->lock);
    int state = c->state;
    spinlock_release(&c->lock);

    return container_state_name(state);
}

/*
 * container_state_query — Query container state in a structured format.
 *
 * Fills an oci_state structure with the current container state and
 * metadata.  This is the programmatic equivalent of reading state.json.
 *
 * Returns 0 on success, negative errno on error.
 */
int container_state_query(struct container *c, struct oci_state *out)
{
    if (!c || !c->in_use || !out)
        return -EINVAL;

    memset(out, 0, sizeof(*out));

    spinlock_acquire(&c->lock);

    out->state        = c->state;
    out->init_pid     = c->init_pid;
    out->memory_limit = c->memory_limit;
    out->cpu_shares   = c->cpu_shares;
    out->cpu_quota_us = c->cpu_quota_us;
    out->cpu_period_us = c->cpu_period_us;
    out->pids_limit   = c->pids_limit;

    /* Copy the state name string */
    const char *name = container_state_names[c->state];
    if (name) {
        size_t name_len = strlen(name);
        if (name_len >= sizeof(out->state_name))
            name_len = sizeof(out->state_name) - 1;
        memcpy(out->state_name, name, name_len);
        out->state_name[name_len] = '\0';
    }

    /* Copy the container ID */
    size_t id_len = strlen(c->id);
    if (id_len >= sizeof(out->container_id))
        id_len = sizeof(out->container_id) - 1;
    memcpy(out->container_id, c->id, id_len);
    out->container_id[id_len] = '\0';

    spinlock_release(&c->lock);

    return 0;
}

/* ── Integration: persist state on transition ──────────────────────── */

/*
 * Persist the current container state to state.json.
 *
 * Called automatically from container_set_state() after every valid state
 * transition.  Failures are logged but do NOT block the state transition
 * — the in-memory state is authoritative, and the JSON file is a
 * convenience for external tooling.
 *
 * Returns 0 on success, negative errno on write failure.
 */
int container_persist_state(struct container *c)
{
    return container_write_state_json(c);
}

/* ── Stub: state_save ─────────────────────────────── */
int state_save(const char *name, const void *state, size_t len)
{
    (void)name;
    (void)state;
    (void)len;
    kprintf("[container] state_save: not yet implemented\n");
    return 0;
}
/* ── Stub: state_load ─────────────────────────────── */
int state_load(const char *name, void *state, size_t *len)
{
    (void)name;
    (void)state;
    (void)len;
    kprintf("[container] state_load: not yet implemented\n");
    return 0;
}
/* ── Stub: state_delete ─────────────────────────────── */
int state_delete(const char *name)
{
    (void)name;
    kprintf("[container] state_delete: not yet implemented\n");
    return 0;
}

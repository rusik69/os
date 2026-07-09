#ifndef OCI_SPEC_H
#define OCI_SPEC_H

#include "types.h"
#include "string.h"
#include "vfs.h"  /* struct vfs_mount_data */

/*
 * oci_spec.h — OCI runtime-spec config.json data structures
 *
 * Mirrors the relevant subset of the OCI runtime-spec (https://github.com/opencontainers/runtime-spec)
 * for parsing config.json in the in-kernel container runtime.
 *
 * Item C2: config.json parser (OCI runtime-spec)
 */

/* ── Constants ─────────────────────────────────────────────────────── */

#define OCI_MAX_ARGS         32    /* Max process arguments */
#define OCI_MAX_ENV          64    /* Max environment variables */
#define OCI_MAX_MOUNTS       32    /* Max mount entries */
#define OCI_MAX_RLIMITS      16    /* Max resource limits */
#define OCI_MAX_CAPS         32    /* Max capability strings */
#define OCI_MAX_NAMESPACES   16    /* Max namespace entries */
#define OCI_MAX_GIDS         16    /* Max supplementary GIDs */
#define OCI_STR_LEN          256   /* Max string length for fields */
#define OCI_PATH_LEN         512   /* Max path length */

/* ── Capability string names ───────────────────────────────────────── */

#define OCI_CAP_NET_RAW       "CAP_NET_RAW"
#define OCI_CAP_DAC_OVERRIDE  "CAP_DAC_OVERRIDE"
#define OCI_CAP_NET_BIND      "CAP_NET_BIND_SERVICE"
#define OCI_CAP_SYS_ADMIN     "CAP_SYS_ADMIN"
#define OCI_CAP_SYS_PTRACE    "CAP_SYS_PTRACE"
#define OCI_CAP_CHOWN         "CAP_CHOWN"
#define OCI_CAP_FOWNER        "CAP_FOWNER"
#define OCI_CAP_SETUID        "CAP_SETUID"
#define OCI_CAP_SETGID        "CAP_SETGID"
#define OCI_CAP_KILL          "CAP_KILL"
#define OCI_CAP_NET_ADMIN     "CAP_NET_ADMIN"
#define OCI_CAP_SYS_NICE      "CAP_SYS_NICE"
#define OCI_CAP_SYS_RESOURCE  "CAP_SYS_RESOURCE"
#define OCI_CAP_AUDIT_WRITE   "CAP_AUDIT_WRITE"
#define OCI_CAP_SETFCAP       "CAP_SETFCAP"
#define OCI_CAP_NET_RAW_VAL      (1U << 13)
#define OCI_CAP_DAC_OVERRIDE_VAL (1U << 0)
#define OCI_CAP_NET_BIND_VAL     (1U << 10)
#define OCI_CAP_SYS_ADMIN_VAL    (1U << 21)
#define OCI_CAP_SYS_PTRACE_VAL   (1U << 19)
#define OCI_CAP_CHOWN_VAL        (1U << 3)
#define OCI_CAP_FOWNER_VAL       (1U << 4)
#define OCI_CAP_SETUID_VAL       (1U << 6)
#define OCI_CAP_SETGID_VAL       (1U << 5)
#define OCI_CAP_KILL_VAL         (1U << 8)
#define OCI_CAP_NET_ADMIN_VAL    (1U << 12)
#define OCI_CAP_SYS_NICE_VAL     (1U << 23)
#define OCI_CAP_SYS_RESOURCE_VAL (1U << 24)
#define OCI_CAP_AUDIT_WRITE_VAL  (1U << 29)
#define OCI_CAP_SETFCAP_VAL      (1U << 31)

/* ── Namespace type strings ────────────────────────────────────────── */

#define OCI_NS_PID      "pid"
#define OCI_NS_NETWORK  "network"
#define OCI_NS_MOUNT    "mount"
#define OCI_NS_UTS      "uts"
#define OCI_NS_IPC      "ipc"
#define OCI_NS_USER     "user"
#define OCI_NS_CGROUP   "cgroup"
#define OCI_NS_TIME     "time"

/* Namespace type to CLONE_NEW* mapping */
#define OCI_CLONE_NEWPID    0x02000000
#define OCI_CLONE_NEWNET    0x40000000
#define OCI_CLONE_NEWNS     0x00020000
#define OCI_CLONE_NEWUTS    0x04000000
#define OCI_CLONE_NEWIPC    0x08000000
#define OCI_CLONE_NEWUSER   0x10000000
#define OCI_CLONE_NEWCGROUP 0x02000000
#define OCI_CLONE_NEWTIME   0x00000080

/* ── RLIMIT type strings ───────────────────────────────────────────── */

#define OCI_RLIMIT_NOFILE   "RLIMIT_NOFILE"
#define OCI_RLIMIT_NPROC    "RLIMIT_NPROC"
#define OCI_RLIMIT_STACK    "RLIMIT_STACK"
#define OCI_RLIMIT_FSIZE    "RLIMIT_FSIZE"
#define OCI_RLIMIT_DATA     "RLIMIT_DATA"
#define OCI_RLIMIT_AS       "RLIMIT_AS"
#define OCI_RLIMIT_CORE     "RLIMIT_CORE"
#define OCI_RLIMIT_RTPRIO   "RLIMIT_RTPRIO"
#define OCI_RLIMIT_RTTIME   "RLIMIT_RTTIME"

/* ── Data structures ───────────────────────────────────────────────── */

/* OCI process/user */
struct oci_user {
    uint32_t uid;
    uint32_t gid;
    uint32_t additional_gids[OCI_MAX_GIDS];
    int      num_additional_gids;
};

/* OCI process capabilities */
struct oci_caps {
    uint32_t effective;    /* bitmask of effective capabilities */
    uint32_t bounding;     /* bitmask of bounding capabilities  */
    uint32_t permitted;    /* bitmask of permitted capabilities  */
    uint32_t inheritable;  /* bitmask of inheritable capabilities */
};

/* OCI resource limit */
struct oci_rlimit {
    char     type[32];     /* e.g. "RLIMIT_NOFILE" */
    uint64_t soft;
    uint64_t hard;
};

/* OCI mount */
struct oci_mount {
    char destination[OCI_PATH_LEN];
    char type[64];
    char source[OCI_PATH_LEN];
    char options[256];
    int  num_options;
};

/* OCI namespace */
struct oci_namespace {
    char type[32];         /* e.g. "pid", "network", "mount" */
    char path[OCI_PATH_LEN]; /* optional: /proc/PID/ns/... */
};

/* OCI process */
struct oci_process {
    struct oci_user  user;
    struct oci_caps  caps;
    char  args[OCI_MAX_ARGS][OCI_STR_LEN];
    int   num_args;
    char  env[OCI_MAX_ENV][OCI_STR_LEN];
    int   num_env;
    char  cwd[OCI_PATH_LEN];
    char  hostname[OCI_STR_LEN];
    int   terminal;
    int   no_new_privs;
    struct oci_rlimit rlimits[OCI_MAX_RLIMITS];
    int   num_rlimits;
};

/* OCI root */
struct oci_root {
    char path[OCI_PATH_LEN];
    int  readonly;
};

/* OCI Linux resources */
struct oci_linux_resources {
    uint64_t memory_limit;     /* bytes, 0 = unlimited */
    uint64_t memory_reservation;
    uint64_t memory_swap;
    uint64_t cpu_shares;
    uint64_t cpu_quota_us;
    uint64_t cpu_period_us;
    uint64_t cpu_cpus;         /* CPU set length */
    uint64_t pids_limit;
};

/* OCI Linux */
struct oci_linux {
    struct oci_linux_resources resources;
    struct oci_namespace namespaces[OCI_MAX_NAMESPACES];
    int   num_namespaces;
    char  cgroups_path[OCI_PATH_LEN];
};

/* ── Top-level parsed config ───────────────────────────────────────── */

struct oci_config {
    char  oci_version[32];
    struct oci_process process;
    struct oci_root    root;
    struct oci_mount   mounts[OCI_MAX_MOUNTS];
    int   num_mounts;
    struct oci_linux   linux;
    char  hostname[OCI_STR_LEN];
    char  err_msg[256];       /* error message from last parse failure */
};

/* ── Capability name-to-bit conversion ─────────────────────────────── */

/**
 * oci_cap_name_to_bit - Convert a capability string name to its bit value.
 * Returns the bit mask (0-31), or 0 if unknown.
 */
static inline uint32_t oci_cap_name_to_bit(const char *name)
{
    if (!name) return 0;
    if (strcmp(name, OCI_CAP_NET_RAW)      == 0) return OCI_CAP_NET_RAW_VAL;
    if (strcmp(name, OCI_CAP_DAC_OVERRIDE) == 0) return OCI_CAP_DAC_OVERRIDE_VAL;
    if (strcmp(name, OCI_CAP_NET_BIND)     == 0) return OCI_CAP_NET_BIND_VAL;
    if (strcmp(name, OCI_CAP_SYS_ADMIN)    == 0) return OCI_CAP_SYS_ADMIN_VAL;
    if (strcmp(name, OCI_CAP_SYS_PTRACE)   == 0) return OCI_CAP_SYS_PTRACE_VAL;
    if (strcmp(name, OCI_CAP_CHOWN)        == 0) return OCI_CAP_CHOWN_VAL;
    if (strcmp(name, OCI_CAP_FOWNER)       == 0) return OCI_CAP_FOWNER_VAL;
    if (strcmp(name, OCI_CAP_SETUID)       == 0) return OCI_CAP_SETUID_VAL;
    if (strcmp(name, OCI_CAP_SETGID)       == 0) return OCI_CAP_SETGID_VAL;
    if (strcmp(name, OCI_CAP_KILL)         == 0) return OCI_CAP_KILL_VAL;
    if (strcmp(name, OCI_CAP_NET_ADMIN)    == 0) return OCI_CAP_NET_ADMIN_VAL;
    if (strcmp(name, OCI_CAP_SYS_NICE)     == 0) return OCI_CAP_SYS_NICE_VAL;
    if (strcmp(name, OCI_CAP_SYS_RESOURCE) == 0) return OCI_CAP_SYS_RESOURCE_VAL;
    if (strcmp(name, OCI_CAP_AUDIT_WRITE)  == 0) return OCI_CAP_AUDIT_WRITE_VAL;
    if (strcmp(name, OCI_CAP_SETFCAP)      == 0) return OCI_CAP_SETFCAP_VAL;
    return 0; /* unknown capability */
}

/* ── Namespace type-to-flag conversion ─────────────────────────────── */

/**
 * oci_ns_type_to_flag - Convert a namespace type string to CLONE_NEW* flag.
 * Returns the clone flag value, or 0 if unknown.
 */
static inline uint64_t oci_ns_type_to_flag(const char *type)
{
    if (!type) return 0;
    if (strcmp(type, OCI_NS_PID)     == 0) return OCI_CLONE_NEWPID;
    if (strcmp(type, OCI_NS_NETWORK) == 0) return OCI_CLONE_NEWNET;
    if (strcmp(type, OCI_NS_MOUNT)   == 0) return OCI_CLONE_NEWNS;
    if (strcmp(type, OCI_NS_UTS)     == 0) return OCI_CLONE_NEWUTS;
    if (strcmp(type, OCI_NS_IPC)     == 0) return OCI_CLONE_NEWIPC;
    if (strcmp(type, OCI_NS_USER)    == 0) return OCI_CLONE_NEWUSER;
    if (strcmp(type, OCI_NS_CGROUP)  == 0) return OCI_CLONE_NEWCGROUP;
    if (strcmp(type, OCI_NS_TIME)    == 0) return OCI_CLONE_NEWTIME;
    return 0;
}

/* ── Top-level API ─────────────────────────────────────────────────── */

/**
 * oci_config_parse - Parse a config.json buffer into an oci_config struct.
 *
 * @config:   Output config struct to populate.
 * @json:     NUL-terminated JSON string from config.json.
 * @size:     Size of the JSON buffer in bytes (excluding NUL terminator).
 *
 * Parses the OCI runtime-spec config.json and populates @config.
 * On error, sets config->err_msg with a descriptive message.
 *
 * Returns 0 on success, -1 on parse error.
 */
int oci_config_parse(struct oci_config *config, const char *json, uint64_t size);

/**
 * oci_config_free - Release any resources allocated during parsing.
 */
void oci_config_free(struct oci_config *config);

/**
 * oci_config_read_file - Read and parse config.json from a filesystem path.
 *
 * @config:  Output config struct.
 * @path:    Full path to config.json.
 *
 * Returns 0 on success, -errno on file error, -1 on parse error.
 */
int oci_config_read_file(struct oci_config *config, const char *path);

/* ═══════════════════════════════════════════════════════════════════════
 *  OCI State — Container runtime state query (Item C7)
 * ═══════════════════════════════════════════════════════════════════════ */

/**
 * struct oci_state — Container state information for external query.
 *
 * Returned by container_state_query() to provide a structured view of
 * the current container state and resource parameters.
 *
 * Fields mirror the OCI runtime-spec state.json schema.
 */
struct oci_state {
    char      container_id[64];        /* Container ID string                */
    char      state_name[16];          /* Human-readable state name          */
    int       state;                   /* Numeric state (CONTAINER_STATE_*)  */
    uint32_t  init_pid;                /* PID of container init process      */
    uint64_t  memory_limit;            /* Memory limit in bytes              */
    uint64_t  cpu_shares;              /* CPU shares (relative weight)       */
    uint64_t  cpu_quota_us;            /* CPU quota in µs per period         */
    uint64_t  cpu_period_us;           /* CPU period in µs                   */
    uint32_t  pids_limit;              /* Max PIDs (0 = unlimited)           */
};

#endif /* OCI_SPEC_H */

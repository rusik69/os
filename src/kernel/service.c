/* service.c — service registry: start/stop/log for kernel network services */

#include "service.h"
#include "fs.h"
#include "printf.h"
#include "string.h"
#include "rtc.h"

static struct service services[SERVICE_MAX];
static int nservices = 0;

/* ── Filesystem directory structure ───────────────────────────────────────── */

/*
 * Prepare the standard directory tree on the filesystem.
 *
 *  /etc          — configuration files
 *  /etc/services — one line per service: "<name> <enabled>"
 *  /var          — variable data
 *  /var/log      — service log files
 *  /var/run      — PID / state files
 *  /tmp/www      — httpd document root (HTTPD_ROOT_DIR)
 *
 * Directories that already exist are silently skipped.
 */
void service_init(void) {
    static const char *dirs[] = {
        "/etc",
        "/var",
        "/var/log",
        "/var/run",
        "/tmp",
        "/tmp/www",
    };
    for (int i = 0; i < (int)(sizeof(dirs) / sizeof(dirs[0])); i++) {
        uint32_t sz; uint8_t tp;
        if (fs_stat(dirs[i], &sz, &tp) < 0)
            fs_create(dirs[i], FS_TYPE_DIR);
    }

    /* Write default /etc/services configuration (name enabled) */
    const char *svcconf = "/etc/services";
    {
        uint32_t sz; uint8_t tp;
        if (fs_stat(svcconf, &sz, &tp) < 0) {
            const char cfg[] =
                "# /etc/services — service configuration\n"
                "# Format: <name> <enabled>\n"
                "telnetd enabled\n"
                "httpd   enabled\n";
            fs_write_file(svcconf, cfg, strlen(cfg));
        }
    }

    /* Write a default index page for the HTTP server if not present */
    const char *index = "/index.html";
    {
        uint32_t sz; uint8_t tp;
        if (fs_stat(index, &sz, &tp) < 0) {
            const char html[] =
                "<html><body><h1>OS Web Server</h1>"
                "<p>This is the default page served by the built-in HTTP server.</p>"
                "</body></html>\n";
            fs_write_file(index, html, strlen(html));
        }
    }
}

/* ── Internal helpers ─────────────────────────────────────────────────────── */

static void log_line(struct service *svc, const char *msg) {
    /* Format:  [HH:MM:SS] msg\n  */
    char line[128];
    int pos = 0;

    struct rtc_time t;
    rtc_get_time(&t);
    {
        /* [HH:MM:SS] */
        line[pos++] = '[';
        line[pos++] = '0' + t.hour / 10;
        line[pos++] = '0' + t.hour % 10;
        line[pos++] = ':';
        line[pos++] = '0' + t.minute / 10;
        line[pos++] = '0' + t.minute % 10;
        line[pos++] = ':';
        line[pos++] = '0' + t.second / 10;
        line[pos++] = '0' + t.second % 10;
        line[pos++] = ']';
        line[pos++] = ' ';
    }

    int mlen = strlen(msg);
    if (mlen > (int)(sizeof(line) - pos - 2))
        mlen = (int)(sizeof(line) - pos - 2);
    memcpy(line + pos, msg, mlen);
    pos += mlen;
    line[pos++] = '\n';

    fs_append(svc->log_path, line, pos);
}

/* ── Public API ───────────────────────────────────────────────────────────── */

int service_register(const char *name, int (*start)(void), void (*stop)(void)) {
    if (nservices >= SERVICE_MAX) return -1;
    if (service_find(name)) return -1; /* already registered */

    struct service *svc = &services[nservices++];
    strncpy(svc->name, name, SERVICE_NAME_MAX - 1);
    svc->name[SERVICE_NAME_MAX - 1] = '\0';
    svc->state = SERVICE_STOPPED;
    svc->start = start;
    svc->stop  = stop;

    /* Build log path: /var/log/<name>.log */
    strncpy(svc->log_path, "/var/log/", sizeof(svc->log_path) - 1);
    strncat(svc->log_path, name, sizeof(svc->log_path) - strlen(svc->log_path) - 5);
    strncat(svc->log_path, ".log", sizeof(svc->log_path) - strlen(svc->log_path) - 1);

    return 0;
}

/* ── /etc/services config writer ──────────────────────────────────────────── */

/* Rewrite /etc/services to reflect current enable/disable state of all
 * registered services.  Called after every start/stop. */
static void write_etc_services(void) {
    /* Build file content in a local buffer */
    static char buf[512];
    int pos = 0;
    const char *hdr =
        "# /etc/services — service configuration\n"
        "# Format: <name> <enabled>\n";
    int hlen = strlen(hdr);
    if (hlen < (int)(sizeof(buf) - pos - 1)) {
        memcpy(buf + pos, hdr, hlen);
        pos += hlen;
    }
    for (int i = 0; i < nservices && pos < (int)(sizeof(buf) - 30); i++) {
        const char *state_str = (services[i].state == SERVICE_RUNNING)
                                ? "enabled\n" : "disabled\n";
        int nlen = strlen(services[i].name);
        memcpy(buf + pos, services[i].name, nlen);
        pos += nlen;
        buf[pos++] = ' ';
        int slen = strlen(state_str);
        memcpy(buf + pos, state_str, slen);
        pos += slen;
    }
    fs_write_file("/etc/services", buf, (uint32_t)pos);
}

int service_start(const char *name) {
    struct service *svc = service_find(name);
    if (!svc) { kprintf("service: unknown service '%s'\n", name); return -1; }
    if (svc->state == SERVICE_RUNNING) {
        kprintf("service: %s is already running\n", name);
        return -1;
    }
    int rc = svc->start();
    if (rc == 0) {
        svc->state = SERVICE_RUNNING;
        log_line(svc, "started");
        write_etc_services();
        kprintf("[svc] %s started\n", name);
    } else {
        log_line(svc, "start failed");
        kprintf("[svc] %s failed to start (rc=%d)\n", name, (int64_t)rc);
    }
    return rc;
}

int service_stop(const char *name) {
    struct service *svc = service_find(name);
    if (!svc) { kprintf("service: unknown service '%s'\n", name); return -1; }
    if (svc->state == SERVICE_STOPPED) {
        kprintf("service: %s is already stopped\n", name);
        return -1;
    }
    svc->stop();
    svc->state = SERVICE_STOPPED;
    log_line(svc, "stopped");
    write_etc_services();
    kprintf("[svc] %s stopped\n", name);
    return 0;
}

int service_count(void) { return nservices; }

struct service *service_get(int idx) {
    if (idx < 0 || idx >= nservices) return NULL;
    return &services[idx];
}

struct service *service_find(const char *name) {
    for (int i = 0; i < nservices; i++)
        if (strcmp(services[i].name, name) == 0) return &services[i];
    return NULL;
}

void service_log(const char *name, const char *msg) {
    struct service *svc = service_find(name);
    if (svc) log_line(svc, msg);
}

/* httpd.c — HTTP/1.1 server: userspace-style persistent task using blocking accept.
 *
 * === Architecture ===
 *
 * This module implements a minimal HTTP/1.1 server as a freestanding kernel
 * task.  It serves static files from a chroot-like directory (HTTPD_ROOT_DIR)
 * and supports GET, HEAD, POST, and DELETE methods.  There is no dependency
 * on libc — string helpers are provided locally.
 *
 * === Request Lifecycle ===
 *
 *   httpd_task()          persistent event loop (kernel thread)
 *     ├─ net_tcp_accept()   blocking accept on port 80
 *     ├─ handle_request()   ═══ MAIN PARSER & ROUTER ═══
 *     │   ├─ Read raw HTTP request (header + body) into recv_buf
 *     │   ├─ Split header from body at "\r\n\r\n"
 *     │   ├─ Parse Content-Length (with overflow guard)
 *     │   ├─ Tokenise request-line:  METHOD SP PATH SP HTTP_VERSION
 *     │   └─ Route by method string:
 *     │       ├─ GET    → handle_get()    serve file from HTTPD_ROOT_DIR
 *     │       ├─ HEAD   → handle_get()    serve headers only
 *     │       ├─ POST   → handle_post()   write body → raw fs path
 *     │       └─ DELETE → handle_delete() remove raw fs path
 *     ├─ net_tcp_close()    close connection
 *     └─ net_poll() ×32    drain pending FIN/ACK
 *
 * === Routing Rules ===
 *
 *   Method   | Target path     | Action
 *   ---------|-----------------|-----------------------------------------
 *   GET      | /path           | File served under HTTPD_ROOT_DIR/path
 *   HEAD     | /path           | Same as GET but no body (headers only)
 *   POST     | /path           | Body written to /path (raw fs, no prefix)
 *   DELETE   | /path           | /path removed from filesystem
 *
 * Path traversal ("..") is rejected for all methods.
 *
 * === Security Considerations ===
 *
 * - Path-traversal check: my_strstr(full_path, "..") before any file operation.
 * - Content-Length overflow guard: arithmetic overflow detection (lines 356-364).
 * - Buffer sizing: recv_buf is 4096 bytes; requests exceeding it are truncated
 *   but still parseable as long as the header boundary is found.
 * - No dynamic allocation: all buffers are stack- or statically-sized.
 */

#include "httpd.h"
#include "net.h"
#include "string.h"
#include "printf.h"
#include "fs.h"
#include "rtc.h"
#include "service.h"
#include "process.h"
#include "scheduler.h"

#define HTTPD_RECV_SIZE 4096
#define HTTPD_RESP_SIZE 8192
#define HTTPD_BODY_SIZE 4096

/* Accept timeout in scheduler ticks — keeps the task responsive to stop */
#define HTTPD_ACCEPT_TIMEOUT 100

/* --- String helpers (no snprintf/strchr/strstr in freestanding) --- */

static char *my_strchr(const char *s, int c) {
    while (*s) { if (*s == (char)c) return (char *)(uintptr_t)s; s++; }
    return 0;
}

static char *my_strstr(const char *h, const char *n) {
    if (!*n) return (char *)(uintptr_t)h;
    for (; *h; h++) {
        const char *a = h, *b = n;
        while (*a && *a == *b) { a++; b++; }
        if (!*b) return (char *)(uintptr_t)h;
    }
    return 0;
}

/* Write a non-negative integer to buf, return ptr past last written char */
static char *u64toa(uint64_t v, char *buf) {
    char tmp[20]; int n = 0;
    do { tmp[n++] = (char)('0' + (v % 10)); v /= 10; } while (v);
    while (--n >= 0) *buf++ = tmp[n];
    return buf;
}

/* Write a string and a newline to buf, return ptr past end */
static char *str_nl(const char *s, char *buf) {
    while (*s) *buf++ = *s++;
    *buf++ = '\r'; *buf++ = '\n';
    return buf;
}

/* --- Content type --- */

static const char *content_type(const char *path) {
    const char *ext = path;
    for (const char *p = path; *p; p++)
        if (*p == '.') ext = p;
    if (!*ext || ext[1] == '\0') return "application/octet-stream";
    ext++;

    if (strcmp(ext, "html") == 0 || strcmp(ext, "htm") == 0) return "text/html";
    if (strcmp(ext, "css")  == 0) return "text/css";
    if (strcmp(ext, "js")   == 0) return "application/javascript";
    if (strcmp(ext, "json") == 0) return "application/json";
    if (strcmp(ext, "txt")  == 0) return "text/plain";
    if (strcmp(ext, "xml")  == 0) return "text/xml";
    if (strcmp(ext, "png")  == 0) return "image/png";
    if (strcmp(ext, "jpg")  == 0 || strcmp(ext, "jpeg") == 0) return "image/jpeg";
    if (strcmp(ext, "gif")  == 0) return "image/gif";
    if (strcmp(ext, "ico")  == 0) return "image/x-icon";
    if (strcmp(ext, "svg")  == 0) return "image/svg+xml";
    if (strcmp(ext, "woff")  == 0) return "font/woff";
    if (strcmp(ext, "woff2") == 0) return "font/woff2";
    if (strcmp(ext, "ttf")  == 0) return "font/ttf";
    if (strcmp(ext, "webp") == 0) return "image/webp";
    return "application/octet-stream";
}

/* --- HTTP date string --- */

static char *http_date(char *buf) {
    struct rtc_time t;
    rtc_get_time(&t);

    /* Day-of-week via Sakamoto's algorithm (0=Sun..6=Sat) */
    static const int dow_tbl[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    int y = t.year; if (t.month < 3) y--;
    int dow = (t.day + y + y/4 - y/100 + y/400 + dow_tbl[t.month - 1]) % 7;
    if (dow < 0) dow += 7;
    static const char *const days[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    static const char *const months[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                   "Jul","Aug","Sep","Oct","Nov","Dec"};

    const char *d = days[dow];
    buf[0] = d[0]; buf[1] = d[1]; buf[2] = d[2]; buf += 3;
    *buf++ = ','; *buf++ = ' ';
    *buf++ = (char)('0' + (t.day / 10)); *buf++ = (char)('0' + (t.day % 10));
    *buf++ = ' ';
    int mi = (t.month >= 1 && t.month <= 12) ? (int)t.month - 1 : 0;
    const char *m = months[mi];
    *buf++ = m[0]; *buf++ = m[1]; *buf++ = m[2];
    *buf++ = ' ';

    /* year (2000-2099) */
    uint16_t yr = (uint16_t)t.year;
    *buf++ = '2'; *buf++ = '0';
    *buf++ = (char)('0' + (yr / 100));
    *buf++ = (char)('0' + ((yr / 10) % 10));
    *buf++ = (char)('0' + (yr % 10));

    *buf++ = ' ';
    *buf++ = (char)('0' + (t.hour / 10)); *buf++ = (char)('0' + (t.hour % 10));
    *buf++ = ':';
    *buf++ = (char)('0' + (t.minute / 10)); *buf++ = (char)('0' + (t.minute % 10));
    *buf++ = ':';
    *buf++ = (char)('0' + (t.second / 10)); *buf++ = (char)('0' + (t.second % 10));
    *buf++ = ' '; *buf++ = 'G'; *buf++ = 'M'; *buf++ = 'T';
    *buf++ = '\r'; *buf++ = '\n';
    return buf;
}

/* --- Response builder --- */

static int build_response(char *resp, int status, const char *status_text,
                          const char *ctype, uint64_t clen,
                          const char *body, int body_len, int head_only) {
    char *p = resp;

    /* Status line */
    *p++ = 'H'; *p++ = 'T'; *p++ = 'T'; *p++ = 'P'; *p++ = '/'; *p++ = '1';
    *p++ = '.'; *p++ = '1'; *p++ = ' ';
    p = u64toa((uint64_t)status, p);
    *p++ = ' ';
    while (*status_text) *p++ = *status_text++;
    *p++ = '\r'; *p++ = '\n';

    /* Date */
    p = http_date(p);

    if (ctype) {
        static const char ct_hdr[] = "Content-Type: ";
        const char *ctp = ct_hdr;
        while (*ctp) *p++ = *ctp++;
        p = str_nl(ctype, p);
    }
    if (clen != (uint64_t)-1) {
        char hdr[64]; char *hp = hdr;
        const char *clp = "Content-Length: ";
        while (*clp) *hp++ = *clp++;
        hp = u64toa(clen, hp);
        *hp++ = '\r'; *hp++ = '\n';
        memcpy(p, hdr, (size_t)(hp - hdr));
        p += (int)(hp - hdr);
    }
    p = str_nl("Connection: close", p);
    p = str_nl("\r\n", p);

    int hdr_len = (int)(p - resp);

    if (body_len > 0 && !head_only) {
        int copy = body_len < (HTTPD_RESP_SIZE - hdr_len) ? body_len : HTTPD_RESP_SIZE - hdr_len;
        if (copy > 0) { memcpy(p, body, copy); p += copy; }
    }
    return (int)(p - resp);
}

static int send_response(int conn_id, int status, const char *status_text,
                         const char *ctype, uint64_t clen,
                         const char *body, int body_len, int head_only) {
    char resp[HTTPD_RESP_SIZE];
    int n = build_response(resp, status, status_text, ctype, clen, body, body_len, head_only);
    return net_tcp_send(conn_id, resp, (uint16_t)n);
}

static void send_error(int conn_id, int status, const char *text, const char *detail) {
    static char body[128];
    char *p = body;
    const char *prefix = "<html><body><h1>";
    while (*prefix) *p++ = *prefix++;
    p = u64toa((uint64_t)status, p);
    *p++ = ' ';
    while (*detail) *p++ = *detail++;
    const char *suffix = "</h1></body></html>";
    while (*suffix) *p++ = *suffix++;
    int n = (int)(p - body);
    send_response(conn_id, status, text, "text/html", (uint64_t)n, body, n, 0);
}

/* Build path under HTTPD_ROOT_DIR */
static void httpd_build_path(const char *path, char *full_path, int max) {
    int pi = 0;
    const char *root = HTTPD_ROOT_DIR;
    for (int i = 0; root[i] && pi < max - 1; i++) full_path[pi++] = root[i];
    if (path[0] == '\0' || strcmp(path, "/") == 0) {
        full_path[pi] = '\0';
        return;
    }
    const char *p = path;
    if (*p == '/') p++;
    if (pi < max - 1 && full_path[pi - 1] != '/') full_path[pi++] = '/';
    for (; *p && pi < max - 1; p++) full_path[pi++] = *p;
    full_path[pi] = '\0';
}

/* --- File handler --- */

/* ── GET/HEAD handler ───────────────────────────────────────────────────
 *
 * Serves a file under HTTPD_ROOT_DIR.  Steps:
 *   (1) Strip query string (?...) from the path.
 *   (2) Map "/" → "/index.html".
 *   (3) Build the full filesystem path by prepending HTTPD_ROOT_DIR.
 *   (4) Reject requests containing ".." (path traversal).
 *   (5) Stat the file; return 404 if not found.
 *   (6) For small files (≤ 4096 bytes) and non-HEAD requests, read the
 *       whole file into a static buffer and send with Content-Length.
 *   (7) For larger files, send headers immediately with Content-Length,
 *       then stream the file in HTTPD_BODY_SIZE chunks.
 */
static void handle_get(int conn_id, char *path, int head_only) {
    /* Strip query string */
    char *q = my_strchr(path, '?');
    if (q) *q = '\0';

    /* Root -> index.html */
    if (path[0] == '\0' || strcmp(path, "/") == 0)
        path = "/index.html";

    char full_path[64];
    httpd_build_path(path, full_path, (int)sizeof(full_path));

    /* Reject path traversal */
    if (my_strstr(full_path, "..") != 0) {
        send_error(conn_id, 403, "Forbidden", "Forbidden");
        return;
    }

    uint32_t fsize = 0;
    uint8_t ftype = 0;
    if (fs_stat(full_path, &fsize, &ftype) < 0 || ftype != FS_TYPE_FILE) {
        send_error(conn_id, 404, "Not Found", "Not Found");
        return;
    }

    char ctype[128];
    strncpy(ctype, content_type(path), sizeof(ctype) - 1);
    ctype[sizeof(ctype) - 1] = '\0';

#define HTTPD_INLINE_MAX  4096
    if (!head_only && fsize <= HTTPD_INLINE_MAX) {
        static char filebuf[HTTPD_INLINE_MAX];
        uint32_t rsize = 0;
        if (fs_read_file(full_path, filebuf, sizeof(filebuf), &rsize) == 0)
            send_response(conn_id, 200, "OK", ctype, (uint64_t)rsize,
                          filebuf, (int)rsize, 0);
        else
            send_error(conn_id, 500, "Internal Server Error", "Read error");
        return;
    }

    send_response(conn_id, 200, "OK", ctype, (uint64_t)fsize, 0, 0, head_only);
    if (head_only) return;

    static char chunk[HTTPD_BODY_SIZE];
    for (uint32_t off = 0; off < fsize; off += HTTPD_BODY_SIZE) {
        uint32_t rlen = fsize - off;
        if (rlen > HTTPD_BODY_SIZE) rlen = HTTPD_BODY_SIZE;
        if (fs_read_file(full_path, chunk, rlen, 0) < 0) break;
        net_tcp_send(conn_id, chunk, (uint16_t)rlen);
    }
}

/* --- Per-request state (stack-allocated in httpd_task) --- */

/* Ensure all parent directories of a path exist */
static void ensure_parent_dirs(const char *path) {
    char buf[64];
    int bi = 0;
    for (int i = 0; path[i] && bi < (int)sizeof(buf) - 1; i++) {
        if (path[i] == '/' && bi > 0) {
            buf[bi] = '\0';
            uint32_t sz; uint8_t tp;
            if (fs_stat(buf, &sz, &tp) != 0 || tp != FS_TYPE_DIR) {
                fs_create(buf, FS_TYPE_DIR);
            }
        }
        buf[bi++] = path[i];
    }
}

/* --- POST handler: write body to file (raw path, no HTTPD_ROOT_DIR) --- */
/* ── POST handler ───────────────────────────────────────────────────────
 *
 * Writes the request body to a raw filesystem path (no HTTPD_ROOT_DIR
 * prefix).  Creates parent directories as needed.  Returns 201 Created
 * on success, or appropriate error on failure.
 */
static void handle_post(int conn_id, const char *path, const char *body, int body_len) {
    /* Strip leading / for raw fs path */
    const char *rp = path;
    while (*rp == '/') rp++;
    char file_path[64];
    file_path[0] = '/';
    int pi = 1;
    for (; *rp && pi < (int)sizeof(file_path) - 1; rp++) file_path[pi++] = *rp;
    file_path[pi] = '\0';

    if (my_strstr(file_path, "..")) { send_error(conn_id, 403, "Forbidden", "Forbidden"); return; }
    if (file_path[1] == '\0') { send_error(conn_id, 400, "Bad Request", "Bad Request"); return; }

    ensure_parent_dirs(file_path);
    fs_create(file_path, FS_TYPE_FILE);
    if (fs_write_file(file_path, body, (uint32_t)body_len) < 0) {
        send_error(conn_id, 500, "Internal Server Error", "Write error"); return;
    }
    static const char created_body[] = "Created";
    send_response(conn_id, 201, "Created", "text/plain",
                  sizeof(created_body)-1, created_body, sizeof(created_body)-1, 0);
}

/* --- DELETE handler: remove file (raw path, no HTTPD_ROOT_DIR) --- */
/* ── DELETE handler ─────────────────────────────────────────────────────
 *
 * Removes a raw filesystem path (no HTTPD_ROOT_DIR prefix).  Returns 200
 * OK with body "Deleted" on success, 404 if the file does not exist, or
 * 403 if the path contains "..".
 */
static void handle_delete(int conn_id, const char *path) {
    const char *rp = path;
    while (*rp == '/') rp++;
    char file_path[64];
    file_path[0] = '/';
    int pi = 1;
    for (; *rp && pi < (int)sizeof(file_path) - 1; rp++) file_path[pi++] = *rp;
    file_path[pi] = '\0';

    if (my_strstr(file_path, "..")) { send_error(conn_id, 403, "Forbidden", "Forbidden"); return; }
    if (file_path[1] == '\0') { send_error(conn_id, 400, "Bad Request", "Bad Request"); return; }

    if (fs_delete(file_path) < 0) {
        send_error(conn_id, 404, "Not Found", "Not Found"); return;
    }
    static const char ok_body[] = "Deleted";
    send_response(conn_id, 200, "OK", "text/plain",
                  sizeof(ok_body)-1, ok_body, sizeof(ok_body)-1, 0);
}

/* ── Request parser & router ────────────────────────────────────────────
 *
 * handle_request() implements the HTTP/1.1 request parsing algorithm:
 *
 *   STEP 1 — RECV: Read client data into recv_buf until "\r\n\r\n" is
 *             found or the buffer is exhausted.
 *   STEP 2 — HEADER SPLIT: Locate "\r\n\r\n" (end-of-headers marker).
 *             Everything before it is the header block; everything after
 *             is (or begins) the message body.
 *   STEP 3 — CONTENT-LENGTH: Scan the header block for
 *             "Content-Length:" / "content-length:", parse the decimal
 *             value with overflow protection, and validate it against
 *             the remaining buffer capacity.
 *   STEP 4 — BODY RECV: If Content-Length indicates more body data
 *             remains, continue reading from the socket.
 *   STEP 5 — METHOD TOKEN: Extract the first space-delimited token from
 *             the request-line → method string (GET/HEAD/POST/DELETE).
 *   STEP 6 — PATH TOKEN:  Extract the second space-delimited token →
 *             request-URI (the path).
 *   STEP 7 — ROUTE: Dispatch to the appropriate handler based on method.
 *
 * Error responses (400 Bad Request, 501 Not Implemented) are generated
 * inline when parsing fails or an unsupported method is used.
 */
static void handle_request(int conn_id) {
    char recv_buf[HTTPD_RECV_SIZE];
    int recv_len = 0;

    /* Read until we have the full HTTP header (\r\n\r\n) */
    while (recv_len < (int)sizeof(recv_buf) - 1) {
        int got = net_tcp_recv(conn_id, recv_buf + recv_len,
                               (uint16_t)(sizeof(recv_buf) - 1 - recv_len), 200);
        if (got <= 0) break;
        recv_len += got;
        recv_buf[recv_len] = '\0';
        if (my_strstr(recv_buf, "\r\n\r\n")) break;
    }

    if (recv_len == 0) return;
    recv_buf[recv_len] = '\0';

    char *hdr_end = my_strstr(recv_buf, "\r\n\r\n");
    if (!hdr_end) { send_error(conn_id, 400, "Bad Request", "Bad Request"); return; }

    /* Parse Content-Length and body_offset BEFORE modifying the buffer with
     * null bytes (my_strstr stops at '\0', so we must search the clean buffer). */
    int content_length = 0;
    int content_length_parsed = 0;
    {
        char *cl = my_strstr(recv_buf, "Content-Length:");
        if (!cl) cl = my_strstr(recv_buf, "content-length:");
        if (cl) {
            cl += 15;
            while (*cl == ' ') cl++;
            while (*cl >= '0' && *cl <= '9') {
                /* Guard against integer overflow from malicious
                 * Content-Length values (e.g., 9999999999999). */
                int digit = *cl - '0';
                if (content_length > (2147483647 - digit) / 10) {
                    send_error(conn_id, 400, "Bad Request",
                               "Invalid Content-Length value");
                    return;
                }
                content_length = content_length * 10 + digit;
                cl++;
                content_length_parsed = 1;
            }
        }
    }
    int body_offset = (int)(hdr_end + 4 - recv_buf);
    /* Validate Content-Length: reject negative values (overflow) and
     * values that would exceed the receive buffer capacity. */
    if (content_length_parsed) {
        if (content_length < 0 ||
            body_offset + content_length > (int)sizeof(recv_buf) - 1) {
            send_error(conn_id, 400, "Bad Request",
                       "Invalid Content-Length");
            return;
        }
    }
    /* If body not yet fully received, read more (before buffer is modified). */
    while (recv_len - body_offset < content_length &&
           recv_len < (int)sizeof(recv_buf) - 1) {
        int got = net_tcp_recv(conn_id, recv_buf + recv_len,
                               (uint16_t)(sizeof(recv_buf) - 1 - recv_len), 200);
        if (got <= 0) break;
        recv_len += got;
        recv_buf[recv_len] = '\0';
    }

    char *req = recv_buf;
    int rlen = (int)(hdr_end - req);
    while (rlen > 0 && (req[rlen-1] == '\r' || req[rlen-1] == '\n')) rlen--;
    req[rlen] = '\0';

    char *sp1 = my_strchr(req, ' ');
    if (!sp1) { send_error(conn_id, 400, "Bad Request", "Bad Request"); return; }
    *sp1 = '\0';
    char method[8];
    strncpy(method, req, sizeof(method) - 1);
    method[sizeof(method) - 1] = '\0';

    char *path_start = sp1 + 1;
    char *sp2 = my_strchr(path_start, ' ');
    if (!sp2) { send_error(conn_id, 400, "Bad Request", "Bad Request"); return; }
    *sp2 = '\0';
    char path[256];
    strncpy(path, path_start, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';

    if (strcmp(method, "GET") != 0 && strcmp(method, "HEAD") != 0 &&
        strcmp(method, "POST") != 0 && strcmp(method, "DELETE") != 0) {
        send_error(conn_id, 501, "Not Implemented", "Not Implemented");
        return;
    }

    kprintf("[httpd] %s %s\n", method, path);
    {
        char logmsg[80];
        strncpy(logmsg, method, sizeof(logmsg) - 1);
        strncat(logmsg, " ", sizeof(logmsg) - strlen(logmsg) - 1);
        strncat(logmsg, path, sizeof(logmsg) - strlen(logmsg) - 1);
        service_log("httpd", logmsg);
    }

    if (strcmp(method, "POST") == 0) {
        char *body = recv_buf + body_offset;
        int body_available = recv_len - body_offset;
        if (body_available < 0) body_available = 0;
        handle_post(conn_id, path, body, body_available > content_length ? content_length : body_available);
        return;
    }

    if (strcmp(method, "DELETE") == 0) {
        handle_delete(conn_id, path);
        return;
    }

    handle_get(conn_id, path, strcmp(method, "HEAD") == 0);
}

/* ── Service lifecycle ──────────────────────────────────────────────────
 *
 * httpd_init()   — called at boot to start the server.
 * httpd_start()  — registers a TCP listen on port 80 (accept-queue mode,
 *                  no per-connection callbacks) and sets the running flag.
 * httpd_stop()   — clears the running flag and unregisters port 80.
 *                  The accept loop sees the flag and idles gracefully.
 */

static volatile int httpd_running = 0;

int httpd_start(void) {
    if (httpd_running) return 0;
    /* Register port without callbacks — accept-queue mode */
    net_tcp_listen(80, (tcp_connect_handler)0,
                       (tcp_data_handler)0,
                       (tcp_close_handler)0);
    httpd_running = 1;
    kprintf("[OK] HTTP server on port 80 (root: %s)\n", HTTPD_ROOT_DIR);
    return 0;
}

void httpd_stop(void) {
    if (!httpd_running) return;
    httpd_running = 0;
    net_tcp_unlisten(80);
    kprintf("[--] HTTP server stopped\n");
}

void httpd_init(void) {
    httpd_start();
}

/* --- Userspace-style task: own kernel process, persistent accept loop --- */

/* ── Main accept loop (userspace-style kernel task) ────────────────────
 *
 * Runs as a persistent kernel thread.  Each iteration:
 *   (1) Check httpd_running flag — idle with scheduler_yield() if stopped.
 *   (2) Block on net_tcp_accept(80, timeout) for a new connection.
 *   (3) On accept, dispatch handle_request() for the full request lifecycle.
 *   (4) Close the connection after the response is sent.
 *   (5) Drain pending packets (net_poll() ×32) to clean up TCP state
 *       before attempting the next accept.
 *
 * The accept timeout (HTTPD_ACCEPT_TIMEOUT = 100 ticks) ensures the task
 * is responsive to stop signals even under idle conditions.
 */
void httpd_task(void) {
    for (;;) {
        if (!httpd_running) {
            /* Service stopped — idle until restarted */
            scheduler_yield();
            continue;
        }
        int conn_id = net_tcp_accept(80, HTTPD_ACCEPT_TIMEOUT);
        if (conn_id < 0) {
            /* No connection yet or stopped during wait — yield then retry */
            scheduler_yield();
            continue;
        }
        kprintf("[httpd] connection accepted conn=%d\n", conn_id);
        handle_request(conn_id);
        net_tcp_close(conn_id);
        /* Drain pending packets (FIN/ACK cleanup) before next accept */
        for (int i = 0; i < 32; i++) net_poll();
        scheduler_yield();
    }
}

/* ── Implement: httpd_handle_request ────────────────── */
static int httpd_handle_request(int conn_fd, const char *request)
{
    if (conn_fd < 0 || !request) {
        kprintf("[httpd] httpd_handle_request: invalid parameter\n");
        return -EINVAL;
    }
    kprintf("[httpd] httpd_handle_request: conn=%d request=%p (stub)\n", conn_fd, (const void *)request);
    return -EOPNOTSUPP;
}
/* ── Implement: httpd_register_handler ────────────────── */
static int httpd_register_handler(const char *path, void *handler)
{
    if (!path || !handler) {
        kprintf("[httpd] httpd_register_handler: NULL parameter\n");
        return -EINVAL;
    }
    kprintf("[httpd] httpd_register_handler: %s handler=%p (stub)\n", path, handler);
    return -EOPNOTSUPP;
}
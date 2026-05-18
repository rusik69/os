/* httpd.c — HTTP server: userspace-style task using blocking accept */

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
    while (*s) { if (*s == (char)c) return (char *)s; s++; }
    return 0;
}

static char *my_strstr(const char *h, const char *n) {
    if (!*n) return (char *)h;
    for (; *h; h++) {
        const char *a = h, *b = n;
        while (*a && *a == *b) { a++; b++; }
        if (!*b) return (char *)h;
    }
    return 0;
}

/* Write a non-negative integer to buf, return ptr past last written char */
static char *u64toa(uint64_t v, char *buf) {
    char tmp[20]; int n = 0;
    do { tmp[n++] = '0' + (v % 10); v /= 10; } while (v);
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
    static volatile int initialized = 0;
    if (!initialized) { rtc_init(); initialized = 1; }
    struct rtc_time t;
    rtc_get_time(&t);

    /* Day-of-week via Sakamoto's algorithm (0=Sun..6=Sat) */
    static const int dow_tbl[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    int y = t.year; if (t.month < 3) y--;
    int dow = (t.day + y + y/4 - y/100 + y/400 + dow_tbl[t.month - 1]) % 7;
    if (dow < 0) dow += 7;
    static const char *days[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    static const char *months[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                   "Jul","Aug","Sep","Oct","Nov","Dec"};

    const char *d = days[dow];
    buf[0] = d[0]; buf[1] = d[1]; buf[2] = d[2]; buf += 3;
    *buf++ = ','; *buf++ = ' ';
    *buf++ = '0' + (t.day / 10); *buf++ = '0' + (t.day % 10);
    *buf++ = ' ';
    const char *m = months[t.month - 1 < 11 ? t.month - 1 : 0];
    *buf++ = m[0]; *buf++ = m[1]; *buf++ = m[2];
    *buf++ = ' ';

    /* year (2000-2099) */
    uint16_t yr = (uint16_t)t.year;
    *buf++ = '2'; *buf++ = '0';
    *buf++ = '0' + (yr / 100);
    *buf++ = '0' + ((yr / 10) % 10);
    *buf++ = '0' + (yr % 10);

    *buf++ = ' ';
    *buf++ = '0' + (t.hour / 10); *buf++ = '0' + (t.hour % 10);
    *buf++ = ':';
    *buf++ = '0' + (t.minute / 10); *buf++ = '0' + (t.minute % 10);
    *buf++ = ':';
    *buf++ = '0' + (t.second / 10); *buf++ = '0' + (t.second % 10);
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

    if (ctype) { p = str_nl(ctype, p); }
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

/* --- File handler --- */

static void handle_get(int conn_id, const char *path, int head_only) {
    /* Strip query string */
    char *q = my_strchr((char *)path, '?');
    if (q) *q = '\0';

    /* Root -> index.html */
    if (path[0] == '\0' || strcmp(path, "/") == 0)
        path = "/index.html";

    /* Build absolute path */
    char full_path[64];
    int pi = 0;
    if (path[0] != '/') full_path[pi++] = '/';
    for (int i = 0; path[i] && pi < 63; i++) full_path[pi++] = path[i];
    full_path[pi] = '\0';

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
        net_tcp_send(conn_id, chunk, rlen);
    }
}

/* --- Per-request state (stack-allocated in httpd_task) --- */

/* --- POST handler: write body to file --- */
static void handle_post(int conn_id, const char *path, const char *body, int body_len) {
    /* Build absolute path */
    char full_path[64]; int pi = 0;
    if (path[0] != '/') full_path[pi++] = '/';
    for (int i = 0; path[i] && pi < 63; i++) full_path[pi++] = path[i];
    full_path[pi] = '\0';

    if (my_strstr(full_path, "..")) { send_error(conn_id, 403, "Forbidden", "Forbidden"); return; }

    /* Create or overwrite */
    fs_create(full_path, FS_TYPE_FILE);  /* ignore error if already exists */
    if (fs_write_file(full_path, body, (uint32_t)body_len) < 0) {
        send_error(conn_id, 500, "Internal Server Error", "Write error"); return;
    }
    static const char created_body[] = "Created";
    send_response(conn_id, 201, "Created", "text/plain",
                  sizeof(created_body)-1, created_body, sizeof(created_body)-1, 0);
}

/* --- DELETE handler --- */
static void handle_delete(int conn_id, const char *path) {
    char full_path[64]; int pi = 0;
    if (path[0] != '/') full_path[pi++] = '/';
    for (int i = 0; path[i] && pi < 63; i++) full_path[pi++] = path[i];
    full_path[pi] = '\0';

    if (my_strstr(full_path, "..")) { send_error(conn_id, 403, "Forbidden", "Forbidden"); return; }

    if (fs_delete(full_path) < 0) {
        send_error(conn_id, 404, "Not Found", "Not Found"); return;
    }
    static const char ok_body[] = "Deleted";
    send_response(conn_id, 200, "OK", "text/plain",
                  sizeof(ok_body)-1, ok_body, sizeof(ok_body)-1, 0);
}

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
    {
        char *cl = my_strstr(recv_buf, "Content-Length:");
        if (!cl) cl = my_strstr(recv_buf, "content-length:");
        if (cl) {
            cl += 15;
            while (*cl == ' ') cl++;
            while (*cl >= '0' && *cl <= '9') {
                content_length = content_length * 10 + (*cl - '0');
                cl++;
            }
        }
    }
    int body_offset = (int)(hdr_end + 4 - recv_buf);
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

/* --- Service interface --- */

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

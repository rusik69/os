/* httpd.c — HTTP server: listen on port 80, parse GET/HEAD, serve files */

#include "httpd.h"
#include "net.h"
#include "string.h"
#include "printf.h"
#include "fs.h"
#include "rtc.h"

#define HTTPD_MAX_CONNS 8
#define HTTPD_RECV_SIZE 4096
#define HTTPD_RESP_SIZE 8192
#define HTTPD_BODY_SIZE 4096

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

/* --- Session --- */

struct httpd_session {
    int conn_id;
    int active;
    char recv_buf[HTTPD_RECV_SIZE];
    int recv_len;
    char method[8];
    char path[256];
};

static struct httpd_session sessions[HTTPD_MAX_CONNS];

static struct httpd_session *find_session(int conn_id) {
    for (int i = 0; i < HTTPD_MAX_CONNS; i++)
        if (sessions[i].active && sessions[i].conn_id == conn_id)
            return &sessions[i];
    return 0;
}

static struct httpd_session *alloc_session(int conn_id) {
    for (int i = 0; i < HTTPD_MAX_CONNS; i++) {
        if (!sessions[i].active) {
            memset(&sessions[i], 0, sizeof(sessions[i]));
            sessions[i].conn_id = conn_id;
            sessions[i].active = 1;
            return &sessions[i];
        }
    }
    return 0;
}

/* --- Response builder --- */

/* Build HTTP response into resp, return total bytes written */
static int build_response(char *resp, int status, const char *status_text,
                          const char *ctype, uint64_t clen,
                          const char *body, int body_len, int head_only) {
    char *p = resp;

    /* Status line: "HTTP/1.1 NNN Text\r\n" */
    *p++ = 'H'; *p++ = 'T'; *p++ = 'T'; *p++ = 'P'; *p++ = '/'; *p++ = '1';
    *p++ = '.'; *p++ = '1'; *p++ = ' ';
    char stat_buf[8];
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
    char sbuf[8];
    p = u64toa((uint64_t)status, p);
    *p++ = ' ';
    while (*detail) *p++ = *detail++;
    const char *suffix = "</h1></body></html>";
    while (*suffix) *p++ = *suffix++;
    int n = (int)(p - body);
    send_response(conn_id, status, text, "text/html", (uint64_t)n, body, n, 0);
}

/* --- Handlers --- */

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

    /* Stream: send headers then chunks */
    send_response(conn_id, 200, "OK", ctype, (uint64_t)-1, 0, 0, head_only);
    if (head_only) return;

    char chunk[HTTPD_BODY_SIZE];
    for (uint32_t off = 0; off < fsize; off += HTTPD_BODY_SIZE) {
        uint32_t rlen = fsize - off;
        if (rlen > HTTPD_BODY_SIZE) rlen = HTTPD_BODY_SIZE;
        if (fs_read_file(full_path, chunk, rlen, 0) < 0) break;
        net_tcp_send(conn_id, chunk, rlen);
    }
}

static void on_connect(int conn_id) {
    if (!alloc_session(conn_id)) { net_tcp_close(conn_id); return; }
    kprintf("[httpd] new connection conn=%d\n", conn_id);
}

static void on_data(int conn_id, const void *data, uint16_t len) {
    struct httpd_session *s = find_session(conn_id);
    if (!s) return;

    /* Too large */
    if (s->recv_len + len >= (int)sizeof(s->recv_buf) - 1) {
        send_error(conn_id, 414, "URI Too Long", "URI Too Long");
        s->active = 0; net_tcp_close(conn_id); return;
    }

    memcpy(s->recv_buf + s->recv_len, data, len);
    s->recv_len += len;
    s->recv_buf[s->recv_len] = '\0';

    /* Look for header end */
    char *hdr_end = my_strstr(s->recv_buf, "\r\n\r\n");
    if (!hdr_end) return;

    /* Extract request line */
    char *req = s->recv_buf;
    int rlen = (int)(hdr_end - req);
    while (rlen > 0 && (req[rlen-1] == '\r' || req[rlen-1] == '\n')) rlen--;
    req[rlen] = '\0';

    /* METHOD SP PATH SP HTTP/... */
    char *sp1 = my_strchr(req, ' ');
    if (!sp1) { send_error(conn_id, 400, "Bad Request", "Bad Request"); goto done; }
    *sp1 = '\0';
    strncpy(s->method, req, sizeof(s->method) - 1);
    s->method[sizeof(s->method) - 1] = '\0';

    char *path = sp1 + 1;
    char *sp2 = my_strchr(path, ' ');
    if (!sp2) { send_error(conn_id, 400, "Bad Request", "Bad Request"); goto done; }
    *sp2 = '\0';
    strncpy(s->path, path, sizeof(s->path) - 1);
    s->path[sizeof(s->path) - 1] = '\0';

    if (strcmp(s->method, "GET") != 0 && strcmp(s->method, "HEAD") != 0) {
        send_error(conn_id, 501, "Not Implemented", "Not Implemented");
        goto done;
    }

    kprintf("[httpd] %s %s\n", s->method, s->path);
    handle_get(conn_id, s->path, strcmp(s->method, "HEAD") == 0);

done:
    s->active = 0;
    net_tcp_close(conn_id);
}

static void on_close(int conn_id) {
    for (int i = 0; i < HTTPD_MAX_CONNS; i++) {
        if (sessions[i].active && sessions[i].conn_id == conn_id) {
            sessions[i].active = 0; return;
        }
    }
}

/* --- Init --- */

void httpd_init(void) {
    memset(sessions, 0, sizeof(sessions));
    net_tcp_listen(80, on_connect, on_data, on_close);
    kprintf("[OK] HTTP server on port 80 (root: %s)\n", HTTPD_ROOT_DIR);
}

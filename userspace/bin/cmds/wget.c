/* wget.c — HTTP download: parse URL, resolve DNS, fetch with net_http_get, save body */
#include "unistd.h"
#include "stdio.h"
#include "string.h"
#include "stdlib.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: wget <url>\n");
        return 1;
    }

    char *url = argv[1];

    /* Parse http://host[:port]/path */
    if (strncmp(url, "http://", 7) != 0) {
        printf("wget: only http:// URLs supported\n");
        return 1;
    }
    url += 7;

    char host[256];
    char path[1024];
    int i = 0;
    while (*url && *url != '/' && *url != ':' && i < 255) {
        host[i++] = *url++;
    }
    host[i] = '\0';

    int port = 80;
    if (*url == ':') {
        url++;
        port = atoi(url);
        while (*url && *url != '/') url++;
    }

    if (*url == '/') {
        int j = 0;
        while (*url && j < 1023) {
            path[j++] = *url++;
        }
        path[j] = '\0';
    } else {
        path[0] = '/';
        path[1] = '\0';
    }

    int ip = net_dns(host);
    if (ip < 0) {
        printf("wget: could not resolve %s\n", host);
        return 1;
    }

    /* Determine output filename: basename of path, or index.html */
    char *fname = "index.html";
    char *last_slash = strrchr(path, '/');
    if (last_slash && *(last_slash + 1) != '\0') {
        fname = last_slash + 1;
    }

    printf("Connecting to %s:%d...\n", host, port);
    printf("Request: GET %s\n", path);

    char buf[8192];
    int n = net_http_get(host, port, path, buf, sizeof(buf) - 1);
    if (n < 0) {
        printf("wget: HTTP request failed\n");
        return 1;
    }
    buf[n] = '\0';

    /* Find HTTP body after header separator */
    char *body = buf;
    char *sep = strstr(buf, "\r\n\r\n");
    if (sep) {
        body = sep + 4;
    } else {
        sep = strstr(buf, "\n\n");
        if (sep) body = sep + 2;
    }

    int body_len = n - (int)(body - buf);
    if (body_len < 0) body_len = 0;

    printf("Saving to: %s (%d bytes)\n", fname, body_len);

    int fd = open(fname, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        printf("wget: could not open %s for writing\n", fname);
        return 1;
    }

    if (body_len > 0) {
        int written = write(fd, body, body_len);
        (void)written;
    }
    close(fd);

    printf("Done.\n");
    return 0;
}

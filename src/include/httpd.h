#ifndef HTTPD_H
#define HTTPD_H

#define HTTPD_ROOT_DIR "/tmp/www"

/* Legacy boot-time initializer (kept for compat). */
void httpd_init(void);

/* Service-style start / stop.  httpd_start returns 0 on success. */
int  httpd_start(void);
void httpd_stop(void);

#endif

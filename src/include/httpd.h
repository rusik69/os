#ifndef HTTPD_H
#define HTTPD_H

#define HTTPD_ROOT_DIR "/tmp/www"

/* Legacy boot-time initializer (kept for compat). */
void httpd_init(void);

/* Service-style start / stop.  httpd_start returns 0 on success. */
int  httpd_start(void);
void httpd_stop(void);

/* Userspace-style task: run as its own process_create'd kernel process.
 * Call httpd_start() before spawning this task. */
void httpd_task(void);

#endif

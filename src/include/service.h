#ifndef SERVICE_H
#define SERVICE_H

#include "types.h"

/* Service states */
#define SERVICE_STOPPED  0
#define SERVICE_RUNNING  1

/* Maximum number of registered services */
#define SERVICE_MAX 8

/* Maximum length of service name */
#define SERVICE_NAME_MAX 16

/* Log path template: /var/log/<name>.log */
#define SERVICE_LOG_DIR  "/var/log"

struct service {
    char     name[SERVICE_NAME_MAX];
    int      state;           /* SERVICE_STOPPED / SERVICE_RUNNING */
    char     log_path[40];    /* /var/log/<name>.log */
    int    (*start)(void);    /* returns 0 on success */
    void   (*stop)(void);
};

/* Initialize the service subsystem and prepare the filesystem structure */
void service_init(void);

/* Register a service (called at boot by httpd/telnetd init) */
int  service_register(const char *name, int (*start)(void), void (*stop)(void));

/* Start / stop a named service */
int  service_start(const char *name);
int  service_stop(const char *name);

/* Query */
int         service_count(void);
struct service *service_get(int idx);
struct service *service_find(const char *name);

/* Write a line to the service's log file (appends with newline) */
void service_log(const char *name, const char *msg);

#endif

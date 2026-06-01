#ifndef SYSCTL_H
#define SYSCTL_H

#include "types.h"

struct sysctl_entry {
    const char *name;
    int (*read_handler)(char *buf, int max);
    int (*write_handler)(const char *buf, int len);
    void *data;
    int maxlen;
    int mode;
};

/* Register a sysctl entry under /proc/sys/kernel/ */
int sysctl_register(const char *name,
                    int (*read_handler)(char *buf, int max),
                    int (*write_handler)(const char *buf, int len));

/* Initialize sysctl subsystem */
void sysctl_init(void);

/* Read/write sysctl values (called from procfs) */
int sysctl_read(const char *name, char *buf, int max);
int sysctl_write(const char *name, const char *buf, int len);

#endif /* SYSCTL_H */

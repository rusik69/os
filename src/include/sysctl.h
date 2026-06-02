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

/* Hostname accessor for shell prompt and other kernel subsystems */
const char *sysctl_get_hostname(void);

/* Set hostname from NUL-terminated string (reads /etc/hostname at boot) */
void sysctl_set_hostname(const char *name);

/*
 * List all registered sysctl names.
 * Fills names[0..count-1] with up to max_names entry names.
 * Returns the number of entries written.
 */
int sysctl_list_names(char names[][48], int max_names);

#endif /* SYSCTL_H */

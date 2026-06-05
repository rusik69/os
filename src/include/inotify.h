#ifndef INOTIFY_H
#define INOTIFY_H

#include "types.h"
#include "fsnotify.h"

/* Syscall implementations — called from syscall dispatch */
int sys_inotify_init1(int flags);
int sys_inotify_add_watch(int fd, const char *user_path, uint32_t mask);
int sys_inotify_rm_watch(int fd, int wd);

/* FD operations — called from sys_read / sys_poll / sys_close dispatch */
int inotify_read(int fd, void *buf, size_t count);
int inotify_poll(int fd);
int inotify_close(int fd);

/* Delivery hook — called from fsnotify_notify */
void inotify_deliver(const char *path, uint32_t fs_mask);

/* Subsystem init — called from production_subsystems_init */
void inotify_subsystem_init(void);

#endif /* INOTIFY_H */

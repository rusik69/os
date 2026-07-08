#ifndef CHROOT_H
#define CHROOT_H

/* Set a chroot jail for the current process */
int chroot_set(const char *path);

#endif /* CHROOT_H */

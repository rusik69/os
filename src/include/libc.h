#ifndef LIBC_H
#define LIBC_H

#include "types.h"

#define TIMER_FREQ 100

/* File type constants mirrored from fs.h for command-side compatibility. */
#define FS_TYPE_FILE   1
#define FS_TYPE_DIR    2
#define FS_MAX_NAME    28
#define PROCESS_MAX    64

struct vfs_stat {
    uint32_t size;
    uint8_t  type;
};

struct libc_fs_stat_ex {
    uint32_t size;
    uint8_t  type;
    uint16_t uid;
    uint16_t gid;
    uint16_t mode;
};

struct libc_process_info {
    uint32_t pid;
    uint32_t ppid;
    uint8_t state;
    uint8_t is_user;
    uint8_t is_background;
    char name[32];
};

/* Low-level syscall shim used by libc wrappers. */
uint64_t libc_syscall(uint64_t num, uint64_t a1, uint64_t a2,
                      uint64_t a3, uint64_t a4, uint64_t a5);

/* Syscall-backed libc operations used by shell command tools. */
int libc_ata_is_present(void);
uint32_t libc_ata_get_sectors(void);
int libc_ahci_is_present(void);
uint32_t libc_ahci_get_sectors(void);
uint64_t libc_uptime_ticks(void);
uint64_t libc_time_seconds(void);
uint64_t libc_getpid(void);
int libc_kill(uint32_t pid, int sig);
int libc_waitpid(uint32_t pid, int *status);
void libc_sleep_ticks(uint64_t ticks);
int libc_net_is_present(void);
void libc_net_get_mac(uint8_t mac[6]);
void libc_net_get_ip(uint8_t ip[4]);
uint32_t libc_net_get_gateway(void);
uint32_t libc_net_get_mask(void);
uint32_t libc_net_dns_resolve(const char *host);
int libc_net_ping(uint32_t ip);
void libc_net_udp_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
                       const void *data, uint16_t len);
int libc_net_http_get_ex(const char *host, uint16_t port, const char *path,
                         char *buf, int bufsize, int follow_redirects);
int libc_net_arp_list_print(void);
int libc_process_list(struct libc_process_info *out, int max);
void libc_pci_list(void);
void libc_usb_list(void);
void libc_hwinfo_print(void);

int libc_fs_format(void);
int libc_fs_create(const char *path, uint8_t type);
int libc_fs_write_file(const char *path, const void *data, uint32_t size);
int libc_fs_read_file(const char *path, void *buf, uint32_t max_size, uint32_t *out_size);
int libc_fs_delete(const char *path);
int libc_fs_list(const char *path);
int libc_fs_stat(const char *path, uint32_t *size, uint8_t *type);
int libc_fs_stat_ex(const char *path, uint32_t *size, uint8_t *type,
                    uint16_t *uid, uint16_t *gid, uint16_t *mode);
int libc_fs_chmod(const char *path, uint16_t mode);
int libc_fs_chown(const char *path, uint16_t uid, uint16_t gid);
void libc_fs_get_usage(uint32_t *used_inodes, uint32_t *total_inodes,
                       uint32_t *used_blocks, uint32_t *data_start);
int libc_fs_list_names(const char *dir, const char *prefix,
                       char names[][FS_MAX_NAME], int max);

int libc_vfs_read(const char *path, void *buf, uint32_t max, uint32_t *out_size);
int libc_vfs_write(const char *path, const void *data, uint32_t size);
int libc_vfs_stat(const char *path, struct vfs_stat *st);
int libc_vfs_create(const char *path, uint8_t type);
int libc_vfs_unlink(const char *path);
int libc_vfs_readdir(const char *path);

/* Compatibility wrappers so existing command code can be migrated with includes only. */
static inline int ata_is_present(void) { return libc_ata_is_present(); }
static inline uint32_t ata_get_sectors(void) { return libc_ata_get_sectors(); }
static inline int ahci_is_present(void) { return libc_ahci_is_present(); }
static inline uint32_t ahci_get_sectors(void) { return libc_ahci_get_sectors(); }

static inline int fs_format(void) { return libc_fs_format(); }
static inline int fs_create(const char *path, uint8_t type) { return libc_fs_create(path, type); }
static inline int fs_write_file(const char *path, const void *data, uint32_t size) {
    return libc_fs_write_file(path, data, size);
}
static inline int fs_read_file(const char *path, void *buf, uint32_t max_size, uint32_t *out_size) {
    return libc_fs_read_file(path, buf, max_size, out_size);
}
static inline int fs_delete(const char *path) { return libc_fs_delete(path); }
static inline int fs_list(const char *path) { return libc_fs_list(path); }
static inline int fs_stat(const char *path, uint32_t *size, uint8_t *type) {
    return libc_fs_stat(path, size, type);
}
static inline int fs_stat_ex(const char *path, uint32_t *size, uint8_t *type,
                             uint16_t *uid, uint16_t *gid, uint16_t *mode) {
    return libc_fs_stat_ex(path, size, type, uid, gid, mode);
}
static inline int fs_chmod(const char *path, uint16_t mode) { return libc_fs_chmod(path, mode); }
static inline int fs_chown(const char *path, uint16_t uid, uint16_t gid) {
    return libc_fs_chown(path, uid, gid);
}
static inline void fs_get_usage(uint32_t *used_inodes, uint32_t *total_inodes,
                                uint32_t *used_blocks, uint32_t *data_start) {
    libc_fs_get_usage(used_inodes, total_inodes, used_blocks, data_start);
}
static inline int fs_list_names(const char *dir, const char *prefix,
                                char names[][FS_MAX_NAME], int max) {
    return libc_fs_list_names(dir, prefix, names, max);
}

static inline int vfs_read(const char *path, void *buf, uint32_t max, uint32_t *out_size) {
    return libc_vfs_read(path, buf, max, out_size);
}
static inline int vfs_write(const char *path, const void *data, uint32_t size) {
    return libc_vfs_write(path, data, size);
}
static inline int vfs_stat(const char *path, struct vfs_stat *st) {
    return libc_vfs_stat(path, st);
}
static inline int vfs_create(const char *path, uint8_t type) { return libc_vfs_create(path, type); }
static inline int vfs_unlink(const char *path) { return libc_vfs_unlink(path); }
static inline int vfs_readdir(const char *path) { return libc_vfs_readdir(path); }

/* Utility helper used by stat/chmod style tools. */
static inline void fs_mode_str(uint16_t mode, char out[10]) {
    out[0] = (mode & 0400) ? 'r' : '-';
    out[1] = (mode & 0200) ? 'w' : '-';
    out[2] = (mode & 0100) ? 'x' : '-';
    out[3] = (mode & 0040) ? 'r' : '-';
    out[4] = (mode & 0020) ? 'w' : '-';
    out[5] = (mode & 0010) ? 'x' : '-';
    out[6] = (mode & 0004) ? 'r' : '-';
    out[7] = (mode & 0002) ? 'w' : '-';
    out[8] = (mode & 0001) ? 'x' : '-';
    out[9] = '\0';
}

#endif
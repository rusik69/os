#include "libc.h"
#include "syscall.h"

uint64_t libc_syscall(uint64_t num, uint64_t a1, uint64_t a2,
                      uint64_t a3, uint64_t a4, uint64_t a5) {
    return syscall_dispatch(num, a1, a2, a3, a4, a5);
}

int libc_ata_is_present(void) {
    return (int)libc_syscall(SYS_ATA_PRESENT, 0, 0, 0, 0, 0);
}

uint32_t libc_ata_get_sectors(void) {
    return (uint32_t)libc_syscall(SYS_ATA_SECTORS, 0, 0, 0, 0, 0);
}

int libc_ahci_is_present(void) {
    return (int)libc_syscall(SYS_AHCI_PRESENT, 0, 0, 0, 0, 0);
}

uint32_t libc_ahci_get_sectors(void) {
    return (uint32_t)libc_syscall(SYS_AHCI_SECTORS, 0, 0, 0, 0, 0);
}

uint64_t libc_uptime_ticks(void) {
    return libc_syscall(SYS_UPTIME, 0, 0, 0, 0, 0);
}

uint64_t libc_time_seconds(void) {
    return libc_syscall(SYS_TIME, 0, 0, 0, 0, 0);
}

uint64_t libc_getpid(void) {
    return libc_syscall(SYS_GETPID, 0, 0, 0, 0, 0);
}

int libc_kill(uint32_t pid, int sig) {
    return (int)libc_syscall(SYS_KILL, pid, (uint64_t)sig, 0, 0, 0);
}

int libc_waitpid(uint32_t pid, int *status) {
    return (int)libc_syscall(SYS_WAITPID, pid, (uint64_t)(uintptr_t)status, 0, 0, 0);
}

void libc_sleep_ticks(uint64_t ticks) {
    (void)libc_syscall(SYS_SLEEP_TICKS, ticks, 0, 0, 0, 0);
}

int libc_net_is_present(void) {
    return (int)libc_syscall(SYS_NET_PRESENT, 0, 0, 0, 0, 0);
}

void libc_net_get_mac(uint8_t mac[6]) {
    (void)libc_syscall(SYS_NET_GET_MAC, (uint64_t)(uintptr_t)mac, 0, 0, 0, 0);
}

void libc_net_get_ip(uint8_t ip[4]) {
    (void)libc_syscall(SYS_NET_GET_IP, (uint64_t)(uintptr_t)ip, 0, 0, 0, 0);
}

uint32_t libc_net_get_gateway(void) {
    return (uint32_t)libc_syscall(SYS_NET_GET_GW, 0, 0, 0, 0, 0);
}

uint32_t libc_net_get_mask(void) {
    return (uint32_t)libc_syscall(SYS_NET_GET_MASK, 0, 0, 0, 0, 0);
}

uint32_t libc_net_dns_resolve(const char *host) {
    return (uint32_t)libc_syscall(SYS_NET_DNS, (uint64_t)(uintptr_t)host, 0, 0, 0, 0);
}

int libc_net_ping(uint32_t ip) {
    return (int)libc_syscall(SYS_NET_PING, ip, 0, 0, 0, 0);
}

void libc_net_udp_send(uint32_t dst_ip, uint16_t src_port, uint16_t dst_port,
                       const void *data, uint16_t len) {
    (void)libc_syscall(SYS_NET_UDP_SEND, dst_ip, src_port, dst_port,
                       (uint64_t)(uintptr_t)data, len);
}

int libc_net_http_get_ex(const char *host, uint16_t port, const char *path,
                         char *buf, int bufsize, int follow_redirects) {
    return (int)libc_syscall(SYS_NET_HTTP_GET, (uint64_t)(uintptr_t)host, port,
                             (uint64_t)(uintptr_t)path, (uint64_t)(uintptr_t)buf,
                             ((uint64_t)(uint32_t)bufsize << 32) | (uint32_t)follow_redirects);
}

int libc_net_arp_list_print(void) {
    return (int)libc_syscall(SYS_NET_ARP_LIST, 0, 0, 0, 0, 0);
}

int libc_process_list(struct libc_process_info *out, int max) {
    return (int)libc_syscall(SYS_PROC_LIST, (uint64_t)(uintptr_t)out, (uint64_t)max, 0, 0, 0);
}

void libc_pci_list(void) {
    (void)libc_syscall(SYS_PCI_LIST, 0, 0, 0, 0, 0);
}

void libc_usb_list(void) {
    (void)libc_syscall(SYS_USB_LIST, 0, 0, 0, 0, 0);
}

void libc_hwinfo_print(void) {
    (void)libc_syscall(SYS_HWINFO_PRINT, 0, 0, 0, 0, 0);
}

int libc_fs_format(void) {
    return (int)libc_syscall(SYS_FS_FORMAT, 0, 0, 0, 0, 0);
}

int libc_fs_create(const char *path, uint8_t type) {
    return (int)libc_syscall(SYS_FS_CREATE, (uint64_t)(uintptr_t)path, type, 0, 0, 0);
}

int libc_fs_write_file(const char *path, const void *data, uint32_t size) {
    return (int)libc_syscall(SYS_FS_WRITE, (uint64_t)(uintptr_t)path,
                             (uint64_t)(uintptr_t)data, size, 0, 0);
}

int libc_fs_read_file(const char *path, void *buf, uint32_t max_size, uint32_t *out_size) {
    return (int)libc_syscall(SYS_FS_READ, (uint64_t)(uintptr_t)path,
                             (uint64_t)(uintptr_t)buf, max_size,
                             (uint64_t)(uintptr_t)out_size, 0);
}

int libc_fs_delete(const char *path) {
    return (int)libc_syscall(SYS_FS_DELETE, (uint64_t)(uintptr_t)path, 0, 0, 0, 0);
}

int libc_fs_list(const char *path) {
    return (int)libc_syscall(SYS_FS_LIST, (uint64_t)(uintptr_t)path, 0, 0, 0, 0);
}

int libc_fs_stat(const char *path, uint32_t *size, uint8_t *type) {
    uint32_t out[2] = {0, 0};
    int rc = (int)libc_syscall(SYS_FS_STAT, (uint64_t)(uintptr_t)path,
                               (uint64_t)(uintptr_t)out, 0, 0, 0);
    if (rc < 0) return rc;
    if (size) *size = out[0];
    if (type) *type = (uint8_t)out[1];
    return 0;
}

int libc_fs_stat_ex(const char *path, uint32_t *size, uint8_t *type,
                    uint16_t *uid, uint16_t *gid, uint16_t *mode) {
    struct libc_fs_stat_ex st;
    int rc = (int)libc_syscall(SYS_FS_STAT_EX, (uint64_t)(uintptr_t)path,
                               (uint64_t)(uintptr_t)&st, 0, 0, 0);
    if (rc < 0) return rc;
    if (size) *size = st.size;
    if (type) *type = st.type;
    if (uid) *uid = st.uid;
    if (gid) *gid = st.gid;
    if (mode) *mode = st.mode;
    return 0;
}

int libc_fs_chmod(const char *path, uint16_t mode) {
    return (int)libc_syscall(SYS_FS_CHMOD, (uint64_t)(uintptr_t)path, mode, 0, 0, 0);
}

int libc_fs_chown(const char *path, uint16_t uid, uint16_t gid) {
    return (int)libc_syscall(SYS_FS_CHOWN, (uint64_t)(uintptr_t)path, uid, gid, 0, 0);
}

void libc_fs_get_usage(uint32_t *used_inodes, uint32_t *total_inodes,
                       uint32_t *used_blocks, uint32_t *data_start) {
    uint32_t out[4] = {0, 0, 0, 0};
    (void)libc_syscall(SYS_FS_GET_USAGE, (uint64_t)(uintptr_t)out, 0, 0, 0, 0);
    if (used_inodes) *used_inodes = out[0];
    if (total_inodes) *total_inodes = out[1];
    if (used_blocks) *used_blocks = out[2];
    if (data_start) *data_start = out[3];
}

int libc_fs_list_names(const char *dir, const char *prefix,
                       char names[][FS_MAX_NAME], int max) {
    return (int)libc_syscall(SYS_FS_LIST_NAMES, (uint64_t)(uintptr_t)dir,
                             (uint64_t)(uintptr_t)prefix,
                             (uint64_t)(uintptr_t)names, (uint64_t)max, 0);
}

int libc_vfs_read(const char *path, void *buf, uint32_t max, uint32_t *out_size) {
    return (int)libc_syscall(SYS_VFS_READ, (uint64_t)(uintptr_t)path,
                             (uint64_t)(uintptr_t)buf, max,
                             (uint64_t)(uintptr_t)out_size, 0);
}

int libc_vfs_write(const char *path, const void *data, uint32_t size) {
    return (int)libc_syscall(SYS_VFS_WRITE, (uint64_t)(uintptr_t)path,
                             (uint64_t)(uintptr_t)data, size, 0, 0);
}

int libc_vfs_stat(const char *path, struct vfs_stat *st) {
    return (int)libc_syscall(SYS_VFS_STAT, (uint64_t)(uintptr_t)path,
                             (uint64_t)(uintptr_t)st, 0, 0, 0);
}

int libc_vfs_create(const char *path, uint8_t type) {
    return (int)libc_syscall(SYS_VFS_CREATE, (uint64_t)(uintptr_t)path, type, 0, 0, 0);
}

int libc_vfs_unlink(const char *path) {
    return (int)libc_syscall(SYS_VFS_UNLINK, (uint64_t)(uintptr_t)path, 0, 0, 0, 0);
}

int libc_vfs_readdir(const char *path) {
    return (int)libc_syscall(SYS_VFS_READDIR, (uint64_t)(uintptr_t)path, 0, 0, 0, 0);
}
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

int libc_process_set_cap_profile(uint32_t pid, uint32_t profile) {
    return (int)libc_syscall(SYS_PROC_SET_CAP_PROFILE, (uint64_t)pid, (uint64_t)profile, 0, 0, 0);
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

/* User/session syscall wrappers (phase 3 group 1) */
int libc_user_find(const char *username, struct libc_user_entry *out) {
    return (int)libc_syscall(SYS_USER_FIND, (uint64_t)(uintptr_t)username,
                             (uint64_t)(uintptr_t)out, 0, 0, 0);
}

int libc_user_add(const char *username, uint32_t uid, const char *password) {
    return (int)libc_syscall(SYS_USER_ADD, (uint64_t)(uintptr_t)username,
                             uid, (uint64_t)(uintptr_t)password, 0, 0);
}

int libc_user_delete(const char *username) {
    return (int)libc_syscall(SYS_USER_DELETE, (uint64_t)(uintptr_t)username, 0, 0, 0, 0);
}

int libc_user_passwd(const char *username, const char *new_pass) {
    return (int)libc_syscall(SYS_USER_PASSWD, (uint64_t)(uintptr_t)username,
                             (uint64_t)(uintptr_t)new_pass, 0, 0, 0);
}

int libc_session_login(const char *username, const char *password) {
    return (int)libc_syscall(SYS_SESSION_LOGIN, (uint64_t)(uintptr_t)username,
                             (uint64_t)(uintptr_t)password, 0, 0, 0);
}

void libc_session_logout(void) {
    (void)libc_syscall(SYS_SESSION_LOGOUT, 0, 0, 0, 0, 0);
}

struct libc_user_session *libc_session_get(void) {
    return (struct libc_user_session *)libc_syscall(SYS_SESSION_GET, 0, 0, 0, 0, 0);
}

int libc_session_is_root(void) {
    struct libc_user_session *s = libc_session_get();
    return (s && s->logged_in && s->uid == 0) ? 1 : 0;
}

int libc_users_count(void) {
    return (int)libc_syscall(SYS_USERS_COUNT, 0, 0, 0, 0, 0);
}

int libc_users_get_by_index(int idx, struct libc_user_entry *out) {
    return (int)libc_syscall(SYS_USERS_GET_BY_INDEX, (uint64_t)idx,
                             (uint64_t)(uintptr_t)out, 0, 0, 0);
}

/* Return a pointer to the kernel's user table. Since kernel and userspace share
 * the same address space in this simple OS, this is a direct pointer. */
struct libc_user_entry *libc_users_get_table(void) {
    return (struct libc_user_entry *)libc_syscall(SYS_USERS_COUNT, 1, 0, 0, 0, 0);
}

/* Hardware/audio syscall wrappers (phase 3 group 2) */
void libc_speaker_beep(uint32_t frequency, uint32_t duration_ms) {
    (void)libc_syscall(SYS_SPEAKER_BEEP, (uint64_t)frequency, (uint64_t)duration_ms, 0, 0, 0);
}

int libc_rtc_get_time(struct libc_rtc_time *out) {
    return (int)libc_syscall(SYS_RTC_GET_TIME, (uint64_t)(uintptr_t)out, 0, 0, 0, 0);
}

void libc_acpi_shutdown(void) {
    (void)libc_syscall(SYS_ACPI_SHUTDOWN, 0, 0, 0, 0, 0);
}

/* I/O and Memory syscall wrappers (phase 3 group 3a) */
int libc_mouse_get_state(struct libc_mouse_state *out) {
    return (int)libc_syscall(SYS_MOUSE_GET_STATE, (uint64_t)(uintptr_t)out, 0, 0, 0, 0);
}

int libc_serial_read(uint8_t *buf, int max) {
    return (int)libc_syscall(SYS_SERIAL_READ, (uint64_t)(uintptr_t)buf, (uint64_t)max, 0, 0, 0);
}

int libc_serial_write(const uint8_t *buf, int len) {
    return (int)libc_syscall(SYS_SERIAL_WRITE, (uint64_t)(uintptr_t)buf, (uint64_t)len, 0, 0, 0);
}

uint8_t libc_cmos_read_byte(uint8_t addr) {
    return (uint8_t)libc_syscall(SYS_CMOS_READ_BYTE, (uint64_t)addr, 0, 0, 0, 0);
}

int libc_pmm_get_stats(struct libc_pmm_stats *out) {
    return (int)libc_syscall(SYS_PMM_GET_STATS, (uint64_t)(uintptr_t)out, 0, 0, 0, 0);
}

/* Specialized syscall wrappers (phase 3 group 3b) */
int libc_elf_exec(const char *path) {
    return (int)libc_syscall(SYS_ELF_EXEC, (uint64_t)(uintptr_t)path, 0, 0, 0, 0);
}

int libc_script_exec(const char *path) {
    return (int)libc_syscall(SYS_SCRIPT_EXEC, (uint64_t)(uintptr_t)path, 0, 0, 0, 0);
}

int libc_fat32_mount(libc_fat32_disk_t disk, uint32_t part_lba) {
    return (int)libc_syscall(SYS_FAT_MOUNT, (uint64_t)disk, (uint64_t)part_lba, 0, 0, 0);
}

int libc_fat32_is_mounted(void) {
    return (int)libc_syscall(SYS_FAT_IS_MOUNTED, 0, 0, 0, 0, 0);
}

int libc_fat32_list_dir(const char *path, char names[][FAT32_MAX_NAME], int max) {
    return (int)libc_syscall(SYS_FAT_LIST_DIR, (uint64_t)(uintptr_t)path,
                             (uint64_t)(uintptr_t)names, (uint64_t)max, 0, 0);
}

int libc_fat32_read_file(const char *path, void *buf, uint32_t max_size) {
    return (int)libc_syscall(SYS_FAT_READ_FILE, (uint64_t)(uintptr_t)path,
                             (uint64_t)(uintptr_t)buf, (uint64_t)max_size, 0, 0);
}

int libc_fat32_file_size(const char *path) {
    return (int)libc_syscall(SYS_FAT_FILE_SIZE, (uint64_t)(uintptr_t)path, 0, 0, 0, 0);
}

void libc_shell_history_show(void) {
    (void)libc_syscall(SYS_SHELL_HISTORY_SHOW, 0, 0, 0, 0, 0);
}

void libc_shell_read_line(char *buf, int max) {
    (void)libc_syscall(SYS_SHELL_READ_LINE, (uint64_t)(uintptr_t)buf, (uint64_t)max, 0, 0, 0);
}

void libc_shell_var_set(const char *name, const char *value) {
    (void)libc_syscall(SYS_SHELL_VAR_SET, (uint64_t)(uintptr_t)name,
                       (uint64_t)(uintptr_t)value, 0, 0, 0);
}

void libc_shell_exec_cmd(const char *cmd, const char *args) {
    (void)libc_syscall(SYS_SHELL_EXEC_CMD, (uint64_t)(uintptr_t)cmd,
                       (uint64_t)(uintptr_t)args, 0, 0, 0);
}

void libc_vga_set_color(uint8_t fg, uint8_t bg) {
    (void)libc_syscall(SYS_VGA_SET_COLOR, (uint64_t)fg, (uint64_t)bg, 0, 0, 0);
}

int libc_vga_get_fb_info(struct libc_fb_info *out) {
    return (int)libc_syscall(SYS_VGA_GET_FB_INFO, (uint64_t)(uintptr_t)out, 0, 0, 0, 0);
}

int libc_cc_compile(const char *inpath, const char *outpath) {
    return (int)libc_syscall(SYS_CC_COMPILE, (uint64_t)(uintptr_t)inpath,
                             (uint64_t)(uintptr_t)outpath, 0, 0, 0);
}

char libc_keyboard_getchar(void) {
    return (char)libc_syscall(SYS_KEYBOARD_GETCHAR, 0, 0, 0, 0, 0);
}

void libc_shell_history_add(const char *cmd_line) {
    (void)libc_syscall(SYS_SHELL_HISTORY_ADD, (uint64_t)(uintptr_t)cmd_line, 0, 0, 0, 0);
}

int libc_shell_history_count(void) {
    return (int)libc_syscall(SYS_SHELL_HISTORY_COUNT, 0, 0, 0, 0, 0);
}

const char *libc_shell_history_entry(int idx) {
    return (const char *)(uintptr_t)libc_syscall(SYS_SHELL_HISTORY_ENTRY, (uint64_t)idx, 0, 0, 0, 0);
}

void libc_shell_tab_complete_telnet(char *buf, int *len, void *session) {
    (void)libc_syscall(SYS_SHELL_TAB_COMPLETE,
                       (uint64_t)(uintptr_t)buf,
                       (uint64_t)(uintptr_t)len,
                       (uint64_t)(uintptr_t)session,
                       0, 0);
}

void libc_vga_put_entry_at(char c, uint8_t color, uint16_t row, uint16_t col) {
    (void)libc_syscall(SYS_VGA_PUT_ENTRY_AT,
                       (uint64_t)(uint8_t)c,
                       (uint64_t)color,
                       (uint64_t)row,
                       (uint64_t)col,
                       0);
}

void libc_vga_set_cursor(uint16_t row, uint16_t col) {
    (void)libc_syscall(SYS_VGA_SET_CURSOR, (uint64_t)row, (uint64_t)col, 0, 0, 0);
}

void libc_vga_clear(void) {
    (void)libc_syscall(SYS_VGA_CLEAR, 0, 0, 0, 0, 0);
}

int libc_gui_shell_run(void) {
    return (int)libc_syscall(SYS_GUI_SHELL_RUN, 0, 0, 0, 0, 0);
}

/* ---- Heap: malloc/free/calloc/realloc backed by kernel syscalls ---- */

void *libc_malloc(size_t size) {
    return (void *)(uintptr_t)libc_syscall(SYS_MALLOC, (uint64_t)size, 0, 0, 0, 0);
}

void libc_free(void *ptr) {
    (void)libc_syscall(SYS_FREE, (uint64_t)(uintptr_t)ptr, 0, 0, 0, 0);
}

void *libc_realloc(void *ptr, size_t new_size) {
    return (void *)(uintptr_t)libc_syscall(SYS_REALLOC,
        (uint64_t)(uintptr_t)ptr, (uint64_t)new_size, 0, 0, 0);
}

void *libc_calloc(size_t nmemb, size_t size) {
    return (void *)(uintptr_t)libc_syscall(SYS_CALLOC,
        (uint64_t)nmemb, (uint64_t)size, 0, 0, 0);
}






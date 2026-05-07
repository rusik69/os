#ifndef LIBC_H
#define LIBC_H

#include "types.h"

#define TIMER_FREQ 100

/* File type constants mirrored from fs.h for command-side compatibility. */
#define FS_TYPE_FILE   1
#define FS_TYPE_DIR    2
#define FS_MAX_NAME    28
#define PROCESS_MAX    64
#define VGA_WIDTH      80
#define VGA_HEIGHT     25

/* VGA colors mirrored from vga.h for command-side compatibility. */
enum libc_vga_color {
    VGA_BLACK = 0,
    VGA_BLUE = 1,
    VGA_GREEN = 2,
    VGA_CYAN = 3,
    VGA_RED = 4,
    VGA_MAGENTA = 5,
    VGA_BROWN = 6,
    VGA_LIGHT_GREY = 7,
    VGA_DARK_GREY = 8,
    VGA_LIGHT_BLUE = 9,
    VGA_LIGHT_GREEN = 10,
    VGA_LIGHT_CYAN = 11,
    VGA_LIGHT_RED = 12,
    VGA_LIGHT_MAGENTA = 13,
    VGA_YELLOW = 14,
    VGA_WHITE = 15,
};

#define KEY_UP    ((char)0x80)
#define KEY_DOWN  ((char)0x81)
#define KEY_LEFT  ((char)0x82)
#define KEY_RIGHT ((char)0x83)

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

/* Opaque user/session structures for phase 3 group 1. */
#define USER_MAX_NAME    32
#define USER_MAX_PASS    64
#define USER_MAX_HOME    64
#define USER_MAX_ENTRIES 16

struct libc_user_entry {
    char    username[USER_MAX_NAME];
    uint32_t uid;
    uint32_t gid;
    char    home[USER_MAX_HOME];
    uint32_t pw_hash;
    int     active;
};

struct libc_user_session {
    int     logged_in;
    uint32_t uid;
    uint32_t gid;
    char    username[USER_MAX_NAME];
};

/* RTC time structure for phase 3 group 2 */
struct libc_rtc_time {
    uint16_t year;
    uint8_t month;
    uint8_t day;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
};

/* Speaker note frequencies (from speaker.h) for phase 3 group 2 */
#define NOTE_C4  262
#define NOTE_D4  294
#define NOTE_E4  330
#define NOTE_F4  349
#define NOTE_G4  392
#define NOTE_A4  440
#define NOTE_B4  494
#define NOTE_C5  523

/* Mouse state structure for phase 3 group 3a */
struct libc_mouse_state {
    int x;
    int y;
    uint8_t buttons;  /* bit 0=left, bit 1=right, bit 2=middle */
};

/* Memory statistics structure for phase 3 group 3a */
struct libc_pmm_stats {
    uint32_t total_pages;
    uint32_t used_pages;
    uint32_t free_pages;
};

struct libc_fb_info {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint8_t bpp;
    uint8_t is_framebuffer;
};

/* Process capability profile values mirrored from process.h */
#define LIBC_PROC_CAP_NONE    0
#define LIBC_PROC_CAP_DEFAULT 1
#define LIBC_PROC_CAP_TRUSTED 2

/* FAT32 compatibility constants/types for phase 3 group 3b */
#define FAT32_MAX_NAME 256
typedef enum {
    FAT32_DISK_ATA = 0,
    FAT32_DISK_AHCI = 1,
    FAT32_DISK_USB0 = 2,
} libc_fat32_disk_t;

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
int libc_process_set_cap_profile(uint32_t pid, uint32_t profile);
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

/* User/session syscall-backed operations (phase 3 group 1) */
int libc_user_find(const char *username, struct libc_user_entry *out);
int libc_user_add(const char *username, uint32_t uid, const char *password);
int libc_user_delete(const char *username);
int libc_user_passwd(const char *username, const char *new_pass);
int libc_session_login(const char *username, const char *password);
void libc_session_logout(void);
struct libc_user_session *libc_session_get(void);
int libc_session_is_root(void);
int libc_users_count(void);
int libc_users_get_by_index(int idx, struct libc_user_entry *out);
struct libc_user_entry *libc_users_get_table(void);

/* Hardware/audio syscall-backed operations (phase 3 group 2) */
void libc_speaker_beep(uint32_t frequency, uint32_t duration_ms);
int libc_rtc_get_time(struct libc_rtc_time *out);
void libc_acpi_shutdown(void);

/* I/O and Memory syscall-backed operations (phase 3 group 3a) */
int libc_mouse_get_state(struct libc_mouse_state *out);
int libc_serial_read(uint8_t *buf, int max);
int libc_serial_write(const uint8_t *buf, int len);
uint8_t libc_cmos_read_byte(uint8_t addr);
int libc_pmm_get_stats(struct libc_pmm_stats *out);

/* Specialized syscall-backed operations (phase 3 group 3b) */
int libc_elf_exec(const char *path);
int libc_script_exec(const char *path);
int libc_fat32_mount(libc_fat32_disk_t disk, uint32_t part_lba);
int libc_fat32_is_mounted(void);
int libc_fat32_list_dir(const char *path, char names[][FAT32_MAX_NAME], int max);
int libc_fat32_read_file(const char *path, void *buf, uint32_t max_size);
int libc_fat32_file_size(const char *path);

/* Shell-core syscall-backed operations (phase 3 group 3b shell linkage slice) */
void libc_shell_history_show(void);
void libc_shell_read_line(char *buf, int max);
void libc_shell_var_set(const char *name, const char *value);
void libc_shell_exec_cmd(const char *cmd, const char *args);

/* Display syscall-backed operations (phase 3 group 3b color/fbinfo slice) */
void libc_vga_set_color(uint8_t fg, uint8_t bg);
int libc_vga_get_fb_info(struct libc_fb_info *out);

/* Compiler syscall-backed operation (phase 3 group 3b cmd_cc slice) */
int libc_cc_compile(const char *inpath, const char *outpath);
char libc_keyboard_getchar(void);
void libc_shell_history_add(const char *cmd_line);
int libc_shell_history_count(void);
const char *libc_shell_history_entry(int idx);
void libc_shell_tab_complete_telnet(char *buf, int *len, void *session);
void libc_vga_put_entry_at(char c, uint8_t color, uint16_t row, uint16_t col);
void libc_vga_set_cursor(uint16_t row, uint16_t col);
void libc_vga_clear(void);
int libc_gui_shell_run(void);

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

/* User/session compatibility wrappers for command migration */
static inline int user_find(const char *username, struct libc_user_entry *out) {
    return libc_user_find(username, out);
}
static inline int user_add(const char *username, uint32_t uid, const char *password) {
    return libc_user_add(username, uid, password);
}
static inline int user_delete(const char *username) {
    return libc_user_delete(username);
}
static inline int user_passwd(const char *username, const char *new_pass) {
    return libc_user_passwd(username, new_pass);
}
static inline int session_login(const char *username, const char *password) {
    return libc_session_login(username, password);
}
static inline void session_logout(void) {
    libc_session_logout();
}
static inline struct libc_user_session *session_get(void) {
    return libc_session_get();
}
static inline int session_is_root(void) {
    return libc_session_is_root();
}

/* Additional user management helpers */
static inline int users_count(void) {
    return libc_users_count();
}
static inline struct libc_user_entry *users_get_table(void) {
    return libc_users_get_table();
}

/* Type aliases so command code doesn't need changes beyond #include "libc.h" */
#define user_entry libc_user_entry
#define user_session libc_user_session
#define rtc_time libc_rtc_time
#define mouse_state libc_mouse_state
#define pmm_stats libc_pmm_stats
#define fat32_disk_t libc_fat32_disk_t

/* Hardware/audio compatibility wrappers */
static inline void speaker_beep(uint32_t frequency, uint32_t duration_ms) {
    libc_speaker_beep(frequency, duration_ms);
}
static inline int rtc_get_time(struct libc_rtc_time *out) {
    return libc_rtc_get_time(out);
}
static inline void acpi_shutdown(void) {
    libc_acpi_shutdown();
}

/* I/O and memory compatibility wrappers */
static inline int mouse_get_state(struct libc_mouse_state *out) {
    return libc_mouse_get_state(out);
}
static inline int serial_read(uint8_t *buf, int max) {
    return libc_serial_read(buf, max);
}
static inline int serial_write(const uint8_t *buf, int len) {
    return libc_serial_write(buf, len);
}
static inline uint8_t cmos_read_byte(uint8_t addr) {
    return libc_cmos_read_byte(addr);
}
static inline int pmm_get_stats(struct libc_pmm_stats *out) {
    return libc_pmm_get_stats(out);
}

/* Specialized compatibility wrappers */
static inline int elf_exec(const char *path) {
    return libc_elf_exec(path);
}
static inline int script_exec(const char *path) {
    return libc_script_exec(path);
}
static inline int fat32_mount(libc_fat32_disk_t disk, uint32_t part_lba) {
    return libc_fat32_mount(disk, part_lba);
}
static inline int fat32_is_mounted(void) {
    return libc_fat32_is_mounted();
}
static inline int fat32_list_dir(const char *path, char names[][FAT32_MAX_NAME], int max) {
    return libc_fat32_list_dir(path, names, max);
}
static inline int fat32_read_file(const char *path, void *buf, uint32_t max_size) {
    return libc_fat32_read_file(path, buf, max_size);
}
static inline int fat32_file_size(const char *path) {
    return libc_fat32_file_size(path);
}
static inline void vga_set_color(uint8_t fg, uint8_t bg) {
    libc_vga_set_color(fg, bg);
}
static inline int vga_get_fb_info(struct libc_fb_info *out) {
    return libc_vga_get_fb_info(out);
}
static inline int cc_compile(const char *inpath, const char *outpath) {
    return libc_cc_compile(inpath, outpath);
}
static inline char keyboard_getchar(void) {
    return libc_keyboard_getchar();
}
static inline void shell_history_add(const char *cmd_line) {
    libc_shell_history_add(cmd_line);
}
static inline int shell_history_count(void) {
    return libc_shell_history_count();
}
static inline const char *shell_history_entry(int idx) {
    return libc_shell_history_entry(idx);
}
static inline void shell_tab_complete_telnet(char *buf, int *len, void *session) {
    libc_shell_tab_complete_telnet(buf, len, session);
}
static inline void vga_put_entry_at(char c, uint8_t color, uint16_t row, uint16_t col) {
    libc_vga_put_entry_at(c, color, row, col);
}
static inline void vga_set_cursor(uint16_t row, uint16_t col) {
    libc_vga_set_cursor(row, col);
}
static inline void vga_clear(void) {
    libc_vga_clear();
}
static inline int gui_shell_run(void) {
    return libc_gui_shell_run();
}

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
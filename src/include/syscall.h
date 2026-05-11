#ifndef SYSCALL_H
#define SYSCALL_H

#include "types.h"

/* Syscall numbers (Linux-compatible ABI) */
#define SYS_READ    0
#define SYS_WRITE   1
#define SYS_OPEN    2
#define SYS_CLOSE   3
#define SYS_EXIT    4
#define SYS_GETPID  5
#define SYS_KILL    6
#define SYS_BRK     7
#define SYS_STAT    8
#define SYS_MKDIR   9
#define SYS_UNLINK  10
#define SYS_TIME    11
#define SYS_YIELD   12
#define SYS_UPTIME  13

/* Extended syscall set used by command-side libc wrappers. */
#define SYS_FS_FORMAT     100
#define SYS_FS_CREATE     101
#define SYS_FS_WRITE      102
#define SYS_FS_READ       103
#define SYS_FS_DELETE     104
#define SYS_FS_LIST       105
#define SYS_FS_STAT       106
#define SYS_FS_STAT_EX    107
#define SYS_FS_CHMOD      108
#define SYS_FS_CHOWN      109
#define SYS_FS_GET_USAGE  110
#define SYS_FS_LIST_NAMES 111
#define SYS_ATA_PRESENT   112
#define SYS_VFS_READ      113
#define SYS_VFS_WRITE     114
#define SYS_VFS_STAT      115
#define SYS_VFS_CREATE    116
#define SYS_VFS_UNLINK    117
#define SYS_VFS_READDIR   118
#define SYS_WAITPID       119
#define SYS_SLEEP_TICKS   120
#define SYS_ATA_SECTORS   121
#define SYS_AHCI_PRESENT  122
#define SYS_AHCI_SECTORS  123
#define SYS_NET_PRESENT   124
#define SYS_NET_GET_MAC   125
#define SYS_NET_GET_IP    126
#define SYS_NET_GET_GW    127
#define SYS_NET_GET_MASK  128
#define SYS_NET_DNS       129
#define SYS_NET_PING      130
#define SYS_NET_UDP_SEND  131
#define SYS_NET_HTTP_GET  132
#define SYS_NET_ARP_LIST  133
#define SYS_PROC_LIST     134
#define SYS_PCI_LIST      135
#define SYS_USB_LIST      136
#define SYS_HWINFO_PRINT  137

/* User/Session management syscalls (Phase 3 Group 1) */
#define SYS_USER_FIND     138
#define SYS_USER_ADD      139
#define SYS_USER_DELETE   140
#define SYS_USER_PASSWD   141
#define SYS_SESSION_LOGIN  142
#define SYS_SESSION_LOGOUT 143
#define SYS_SESSION_GET   144
#define SYS_USERS_COUNT   145
#define SYS_USERS_GET_BY_INDEX 146

/* Hardware/Audio syscalls (Phase 3 Group 2) */
#define SYS_SPEAKER_BEEP  147
#define SYS_RTC_GET_TIME  148
#define SYS_ACPI_SHUTDOWN 149

/* I/O and Memory syscalls (Phase 3 Group 3a) */
#define SYS_MOUSE_GET_STATE 150
#define SYS_SERIAL_READ     151
#define SYS_SERIAL_WRITE    152
#define SYS_CMOS_READ_BYTE  153
#define SYS_PMM_GET_STATS   154

/* Specialized syscalls (Phase 3 Group 3b) */
#define SYS_ELF_EXEC        155
#define SYS_SCRIPT_EXEC     156
#define SYS_FAT_MOUNT       157
#define SYS_FAT_IS_MOUNTED  158
#define SYS_FAT_LIST_DIR    159
#define SYS_FAT_READ_FILE   160
#define SYS_FAT_FILE_SIZE   161

/* Shell-core syscalls (Phase 3 Group 3b, shell linkage slice) */
#define SYS_SHELL_HISTORY_SHOW 162
#define SYS_SHELL_READ_LINE    163
#define SYS_SHELL_VAR_SET      164
#define SYS_SHELL_EXEC_CMD     165

/* Display syscalls (Phase 3 Group 3b, color/fbinfo slice) */
#define SYS_VGA_SET_COLOR      166
#define SYS_VGA_GET_FB_INFO    167

/* Compiler syscall (Phase 3 Group 3b, cmd_cc slice) */
#define SYS_CC_COMPILE         168

/* Tmux isolation syscalls (Phase 3 Group 3b, cmd_tmux slice) */
#define SYS_KEYBOARD_GETCHAR    169
#define SYS_SHELL_HISTORY_ADD   170
#define SYS_SHELL_HISTORY_COUNT 171
#define SYS_SHELL_HISTORY_ENTRY 172
#define SYS_SHELL_TAB_COMPLETE  173
#define SYS_VGA_PUT_ENTRY_AT    174
#define SYS_VGA_SET_CURSOR      175
#define SYS_VGA_CLEAR           176
#define SYS_GUI_SHELL_RUN       177
#define SYS_PROC_SET_CAP_PROFILE 178

/* Heap syscalls — userspace malloc/free */
#define SYS_MALLOC              179
#define SYS_FREE                180
#define SYS_REALLOC             181
#define SYS_CALLOC              182

void syscall_init(void);

/* Called from assembly stub - dispatches to the right handler */
uint64_t syscall_dispatch(uint64_t num, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5);

#endif

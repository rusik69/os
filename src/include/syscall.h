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

/* TCP server syscalls — userspace can act as a TCP server */
#define SYS_NET_TCP_LISTEN      183  /* listen on port (no callbacks) */
#define SYS_NET_TCP_ACCEPT      184  /* blocking accept → conn_id or -1 */
#define SYS_NET_TCP_SEND_CONN   185  /* send data on conn_id */
#define SYS_NET_TCP_RECV_CONN   186  /* recv data from conn_id */
#define SYS_NET_TCP_CLOSE_CONN  187  /* close conn_id */
#define SYS_NET_TCP_UNLISTEN    188  /* stop listening on port */
#define SYS_NET_TCP_CONNECT     189  /* outbound TCP connect → conn_id or -1 */

/* Mutex syscalls */
#define SYS_MUTEX_INIT          190  /* allocate mutex → id or -1 */
#define SYS_MUTEX_LOCK          191  /* lock(id) */
#define SYS_MUTEX_UNLOCK        192  /* unlock(id) */
#define SYS_MUTEX_DESTROY       193  /* free(id) */

/* Semaphore syscalls */
#define SYS_SEM_INIT            194  /* allocate sem(count) → id or -1 */
#define SYS_SEM_WAIT            195  /* wait/decrement */
#define SYS_SEM_POST            196  /* post/increment */
#define SYS_SEM_DESTROY         197  /* free */

/* UDP server syscalls */
#define SYS_NET_UDP_LISTEN   198  /* listen on port → 0 or -1 */
#define SYS_NET_UDP_RECV     199  /* recv pkt → bytes or -1/0 */
#define SYS_NET_UDP_UNLISTEN 200  /* stop listening */

/* Filesystem extended syscalls */
#define SYS_FS_SYMLINK  201  /* create symlink */
#define SYS_FS_READLINK 202  /* read symlink target */
#define SYS_FS_LSTAT    203  /* stat without following symlinks */

/* Working directory */
#define SYS_CHDIR        204  /* change cwd → 0 or -1 */
#define SYS_GETCWD       205  /* copy cwd into buffer → 0 or -1 */
#define SYS_SETPRIORITY  206  /* set process priority 0-3 */

/* Shared memory (IPC) */
#define SYS_SHM_GET   207  /* get/create segment by key → id */
#define SYS_SHM_AT    208  /* map segment → virt addr */
#define SYS_SHM_DT    209  /* decrement ref count */
#define SYS_SHM_FREE  210  /* free segment */

/* Process fork */
#define SYS_FORK      211  /* fork current process */

/* Network connection list */
#define SYS_NET_CONNLIST  212  /* fill buffer with tcp connection info */

/* Signal handling */
#define SYS_SIGNAL        213  /* register signal handler for current process */

/* File seek/truncate */
#define SYS_LSEEK         214  /* seek within open file (returns new offset) */
#define SYS_TRUNCATE      215  /* truncate file to given length */

/* Raw network send */
#define SYS_RAW_SEND      216  /* send raw Ethernet frame */

/* FD-based read/write (uses open file descriptor offset) */
#define SYS_FD_READ       217  /* read count bytes from fd at current offset */
#define SYS_FD_WRITE      218  /* write count bytes to fd at current offset */

/* Job control / priority management */
#define SYS_SETPRIORITY_PID 219  /* set target process priority */
#define SYS_GETPRIORITY     220  /* get target process priority */
#define SYS_SETPGID         221  /* set process group ID */
#define SYS_GETPGID         222  /* get process group ID */
#define SYS_KILLPG          223  /* send signal to process group */
#define SYS_AC97_PRESENT    224  /* returns 1 if AC97 available */
#define SYS_AC97_BEEP       225  /* play square-wave PCM (freq_hz, duration_ms) */
#define SYS_FAT_WRITE_FILE  226  /* write/create file on mounted FAT volume */
#define SYS_FAT_SYNC        227  /* flush FAT tables to disk */
#define SYS_DOOM_RUN        228  /* start raycast doom game (blocking) */
#define SYS_CC_COMPILE_OBJ  229  /* compile C source to relocatable .o file */
#define SYS_CC_LINK          230  /* link multiple .o files into executable */

/* Clone / threading */
#define SYS_CLONE           231
#define SYS_GETTID          232
#define SYS_TKILL           233
#define SYS_EXECVE          234

/*
 * syscall_dispatch is a kernel-internal function called ONLY from the
 * userspace API: user code and libc must go through the `syscall` instruction.
 * The declaration is kept here so that syscall.c (which defines it) and
 * kernel.c (which includes syscall.h) can see it, but it should never be
 * called directly from libc or application code.
 */
#ifdef KERNEL_INTERNAL
void syscall_init(void);
uint64_t syscall_dispatch(uint64_t num, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5);
#endif

#endif

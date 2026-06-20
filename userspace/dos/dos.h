#ifndef DOS_H
#define DOS_H

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>

/* MZ (MS-DOS) executable header */
struct mz_header {
    uint16_t e_magic;
    uint16_t e_cblp;
    uint16_t e_cp;
    uint16_t e_crlc;
    uint16_t e_cparhdr;
    uint16_t e_minalloc;
    uint16_t e_maxalloc;
    uint16_t e_ss;
    uint16_t e_sp;
    uint16_t e_csum;
    uint16_t e_ip;
    uint16_t e_cs;
    uint16_t e_lfarlc;
    uint16_t e_ovno;
    uint16_t e_res[4];
    uint16_t e_oemid;
    uint16_t e_oeminfo;
    uint16_t e_res2[10];
    uint32_t e_lfanew;
} __attribute__((packed));

#define MZ_MAGIC 0x5A4D
#define DOS_SEGMENT_SIZE 0x10000
#define DOS_CONV_MEM_SIZE 0x100000

struct dos_cpu_state {
    uint16_t ax, bx, cx, dx;
    uint16_t si, di, bp, sp;
    uint16_t cs, ds, es, ss;
    uint16_t ip;
    uint16_t flags;
    int running;
    uint8_t *memory;
    char *file_handles[16];
    int    file_handle_count;
};

#define DOS_ATTR_READ_ONLY  0x01
#define DOS_ATTR_HIDDEN     0x02
#define DOS_ATTR_SYSTEM     0x04
#define DOS_ATTR_VOLUME_ID  0x08
#define DOS_ATTR_DIRECTORY  0x10
#define DOS_ATTR_ARCHIVE    0x20

struct dos_psp {
    uint8_t  int20h[2];
    uint16_t end_of_mem;
    uint8_t  reserved1;
    uint8_t  call_dos[5];
    uint32_t int22_vector;
    uint32_t int23_vector;
    uint32_t int24_vector;
    uint8_t  reserved2[22];
    uint16_t environment_seg;
    uint8_t  reserved3[34];
    uint8_t  int21h_ret[3];
    uint8_t  reserved4[9];
    uint8_t  int21h_call[5];
    uint8_t  cmd_tail[128];
} __attribute__((packed));

#define DOS_FLAG_CF   0x0001
#define DOS_FLAG_PF   0x0004
#define DOS_FLAG_AF   0x0010
#define DOS_FLAG_ZF   0x0040
#define DOS_FLAG_SF   0x0080
#define DOS_FLAG_TF   0x0100
#define DOS_FLAG_IF   0x0200
#define DOS_FLAG_DF   0x0400
#define DOS_FLAG_OF   0x0800

/* Syscall wrappers for kernel API replacements */
#define kprintf printf
#define scheduler_yield() yield()

static inline uint64_t timer_get_ticks(void) {
    struct timespec ts;
    memset(&ts, 0, sizeof(ts));
    clock_gettime(0, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* VFS stat struct for dos */
struct vfs_stat {
    uint64_t st_size;
    int st_mode;
};

static inline int vfs_stat(const char *path, struct vfs_stat *st) {
    struct stat buf;
    if (stat(path, &buf) != 0) return -1;
    st->st_size = buf.st_size;
    st->st_mode = 0;
    return 0;
}

static inline int vfs_read(const char *path, void *buf, uint32_t max_size, uint32_t *out_size) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int n = read(fd, buf, max_size);
    close(fd);
    if (n < 0) return -1;
    *out_size = (uint32_t)n;
    return 0;
}

static inline void *heap_alloc(uint32_t size) { return malloc(size); }
static inline void heap_free(void *p) { free(p); }
#define kmalloc heap_alloc
#define kfree heap_free

static inline int fs_append(const char *path, const void *buf, uint32_t len) {
    int fd = open(path, O_WRONLY | O_APPEND);
    if (fd < 0) return -1;
    int n = write(fd, buf, len);
    close(fd);
    return (n < 0) ? -1 : 0;
}

#endif /* DOS_H */

#ifndef DOS_H
#define DOS_H

#include "types.h"

/* MZ (MS-DOS) executable header */
struct mz_header {
    uint16_t e_magic;       /* 0x00: 0x5A4D ('MZ') */
    uint16_t e_cblp;        /* 0x02: bytes on last page */
    uint16_t e_cp;          /* 0x04: pages in file */
    uint16_t e_crlc;        /* 0x06: relocation count */
    uint16_t e_cparhdr;     /* 0x08: header size in paragraphs */
    uint16_t e_minalloc;    /* 0x0A: min extra paragraphs */
    uint16_t e_maxalloc;    /* 0x0C: max extra paragraphs */
    uint16_t e_ss;          /* 0x0E: initial SS */
    uint16_t e_sp;          /* 0x10: initial SP */
    uint16_t e_csum;        /* 0x12: checksum */
    uint16_t e_ip;          /* 0x14: initial IP */
    uint16_t e_cs;          /* 0x16: initial CS */
    uint16_t e_lfarlc;      /* 0x18: relocation table offset */
    uint16_t e_ovno;        /* 0x1A: overlay number */
    uint16_t e_res[4];      /* 0x1C: reserved */
    uint16_t e_oemid;       /* 0x24: OEM ID */
    uint16_t e_oeminfo;     /* 0x26: OEM info */
    uint16_t e_res2[10];    /* 0x28: reserved */
    uint32_t e_lfanew;      /* 0x3C: PE header offset */
} __attribute__((packed));

#define MZ_MAGIC 0x5A4D

/* DOS memory segment (64KB) */
#define DOS_SEGMENT_SIZE 0x10000
/* Conventional memory size (1MB = 16 segments × 64KB) */
#define DOS_CONV_MEM_SIZE 0x100000

/* DOS CPU register state */
struct dos_cpu_state {
    /* 16-bit general purpose registers */
    uint16_t ax, bx, cx, dx;
    uint16_t si, di, bp, sp;
    /* segment registers */
    uint16_t cs, ds, es, ss;
    /* instruction pointer */
    uint16_t ip;
    /* flags */
    uint16_t flags;
    /* Internal: keep track if the emulator is running */
    int running;
    /* Memory: 1MB conventional memory */
    uint8_t *memory;
    /* DOS file handle table (maps DOS handle → file path) */
    char *file_handles[16];
    int    file_handle_count;
};

/* DOS file attribute constants */
#define DOS_ATTR_READ_ONLY  0x01
#define DOS_ATTR_HIDDEN     0x02
#define DOS_ATTR_SYSTEM     0x04
#define DOS_ATTR_VOLUME_ID  0x08
#define DOS_ATTR_DIRECTORY  0x10
#define DOS_ATTR_ARCHIVE    0x20

/* PSP (Program Segment Prefix) structure */
struct dos_psp {
    uint8_t  int20h[2];         /* 0x00: INT 20h */
    uint16_t end_of_mem;        /* 0x02: end of allocated memory */
    uint8_t  reserved1;         /* 0x04 */
    uint8_t  call_dos[5];       /* 0x05: far call to DOS */
    uint32_t int22_vector;      /* 0x0A: terminate vector */
    uint32_t int23_vector;      /* 0x0E: Ctrl-C vector */
    uint32_t int24_vector;      /* 0x12: critical error vector */
    uint8_t  reserved2[22];     /* 0x16 */
    uint16_t environment_seg;   /* 0x2C: environment segment */
    uint8_t  reserved3[34];     /* 0x2E */
    uint8_t  int21h_ret[3];     /* 0x50: INT 21h return */
    uint8_t  reserved4[9];      /* 0x53 */
    uint8_t  int21h_call[5];    /* 0x5C: far call for INT 21h */
    uint8_t  cmd_tail[128];     /* 0x80: command tail + default DTA */
} __attribute__((packed));

/* Flag bit constants for the emulated CPU */
#define DOS_FLAG_CF   0x0001
#define DOS_FLAG_PF   0x0004
#define DOS_FLAG_AF   0x0010
#define DOS_FLAG_ZF   0x0040
#define DOS_FLAG_SF   0x0080
#define DOS_FLAG_TF   0x0100
#define DOS_FLAG_IF   0x0200
#define DOS_FLAG_DF   0x0400
#define DOS_FLAG_OF   0x0800

#endif /* DOS_H */

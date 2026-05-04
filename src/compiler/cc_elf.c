/* cc_elf.c — ELF64 binary writer for the in-kernel C compiler */

#include "cc.h"
#include "elf.h"
#include "vfs.h"
#include "string.h"
#include "printf.h"
#include "heap.h"

/*
 * Layout:
 *   File offset 0x000  : ELF header (64 bytes)
 *   File offset 0x040  : Program header 0 (code, PT_LOAD RX) (56 bytes)
 *   File offset 0x078  : Program header 1 (data, PT_LOAD RW) (56 bytes)
 *   File offset 0x0B0  : Code bytes
 *   File offset 0x0B0 + code_len (page-aligned to 0x1000) : Data bytes
 *
 *   Virtual address of _start (code[0]) = CC_LOAD_BASE = 0x400000
 *   Virtual address of data[0]          = CC_LOAD_BASE + CC_DATA_OFFSET = 0x401000
 */

#define ELF_HDR_SIZE   64
#define ELF_PHDR_SIZE  56
#define CODE_FILE_OFF  (ELF_HDR_SIZE + 2 * ELF_PHDR_SIZE)  /* 0xB0 = 176 */
#define PAGE_SZ        0x1000U

static void write_le16(uint8_t *p, uint16_t v) {
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
}
static void write_le32(uint8_t *p, uint32_t v) {
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff;
    p[3] = (v >> 24) & 0xff;
}
static void write_le64(uint8_t *p, uint64_t v) {
    write_le32(p, (uint32_t)(v & 0xffffffff));
    write_le32(p + 4, (uint32_t)(v >> 32));
}

int cc_write_elf(CompilerState *cc, const char *outpath) {
    /* Compute sizes */
    uint32_t code_sz = (uint32_t)cc->code_len;
    uint32_t data_sz = (uint32_t)cc->data_len;
    if (data_sz == 0) data_sz = 8; /* at least one page-worth */

    /* data file offset: align code section to PAGE_SZ boundary */
    uint32_t code_file_off = (uint32_t)CODE_FILE_OFF;
    uint32_t data_file_off = (code_file_off + code_sz + PAGE_SZ - 1) & ~(PAGE_SZ - 1);
    uint32_t total_size    = data_file_off + data_sz;

    /* Allocate output buffer */
    uint8_t *buf = (uint8_t *)kmalloc(total_size);
    if (!buf) {
        kprintf("cc: out of memory for ELF output\n");
        return -1;
    }
    memset(buf, 0, total_size);

    /* ---- ELF header ---- */
    uint8_t *hdr = buf;
    /* e_ident */
    hdr[0] = 0x7F; hdr[1] = 'E'; hdr[2] = 'L'; hdr[3] = 'F';
    hdr[4] = ELF_CLASS64;   /* 64-bit */
    hdr[5] = ELF_DATA2LSB;  /* little-endian */
    hdr[6] = 1;              /* ELF version */
    hdr[7] = 0;              /* OS/ABI = System V */
    /* bytes 8-15: padding */
    write_le16(hdr + 16, ET_EXEC);       /* e_type */
    write_le16(hdr + 18, EM_X86_64);    /* e_machine */
    write_le32(hdr + 20, 1);             /* e_version */
    write_le64(hdr + 24, CC_LOAD_BASE); /* e_entry */
    write_le64(hdr + 32, ELF_HDR_SIZE); /* e_phoff */
    write_le64(hdr + 40, 0);            /* e_shoff */
    write_le32(hdr + 48, 0);            /* e_flags */
    write_le16(hdr + 52, ELF_HDR_SIZE); /* e_ehsize */
    write_le16(hdr + 54, ELF_PHDR_SIZE);/* e_phentsize */
    write_le16(hdr + 56, 2);            /* e_phnum */
    write_le16(hdr + 58, 64);           /* e_shentsize */
    write_le16(hdr + 60, 0);            /* e_shnum */
    write_le16(hdr + 62, 0);            /* e_shstrndx */

    /* ---- Program header 0: code (PT_LOAD, RX) ---- */
    uint8_t *ph0 = buf + ELF_HDR_SIZE;
    write_le32(ph0 +  0, 1);               /* p_type = PT_LOAD */
    write_le32(ph0 +  4, 5);               /* p_flags = R|X */
    write_le64(ph0 +  8, code_file_off);   /* p_offset */
    write_le64(ph0 + 16, CC_LOAD_BASE);    /* p_vaddr */
    write_le64(ph0 + 24, CC_LOAD_BASE);    /* p_paddr */
    write_le64(ph0 + 32, code_sz);         /* p_filesz */
    write_le64(ph0 + 40, code_sz);         /* p_memsz */
    write_le64(ph0 + 48, PAGE_SZ);         /* p_align */

    /* ---- Program header 1: data (PT_LOAD, RW) ---- */
    uint8_t *ph1 = buf + ELF_HDR_SIZE + ELF_PHDR_SIZE;
    uint64_t data_vaddr = CC_LOAD_BASE + CC_DATA_OFFSET;
    write_le32(ph1 +  0, 1);               /* p_type = PT_LOAD */
    write_le32(ph1 +  4, 6);               /* p_flags = R|W */
    write_le64(ph1 +  8, data_file_off);   /* p_offset */
    write_le64(ph1 + 16, data_vaddr);      /* p_vaddr */
    write_le64(ph1 + 24, data_vaddr);      /* p_paddr */
    write_le64(ph1 + 32, data_sz);         /* p_filesz */
    write_le64(ph1 + 40, data_sz);         /* p_memsz */
    write_le64(ph1 + 48, PAGE_SZ);         /* p_align */

    /* ---- Copy code and data ---- */
    memcpy(buf + code_file_off, cc->code, code_sz);
    memcpy(buf + data_file_off, cc->data, data_sz);

    /* ---- Write to filesystem ---- */
    int ret = vfs_write(outpath, buf, total_size);
    kfree(buf);

    if (ret < 0) {
        kprintf("cc: failed to write %s\n", outpath);
        return -1;
    }
    kprintf("cc: wrote %u bytes to %s\n", total_size, outpath);
    return 0;
}

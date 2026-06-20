/*
 * dos_int21.c - DOS INT 21h function handler
 *
 * Implements the most common MS-DOS API subfunctions for the emulated
 * 16-bit real-mode environment.
 *
 * Handled AH subfunctions:
 *   0x00  – exit program
 *   0x01  – read char with echo
 *   0x02  – print char in DL
 *   0x07  – read char without echo
 *   0x08  – read char without echo, check Ctrl-C
 *   0x09  – print $-terminated string at DS:DX
 *   0x0A  – buffered input at DS:DX
 *   0x25  – set interrupt vector (ignored)
 *   0x2A  – get system date
 *   0x2C  – get system time
 *   0x30  – get DOS version
 *   0x35  – get interrupt vector
 *   0x3D  – open file
 *   0x3E  – close file
 *   0x3F  – read from file
 *   0x40  – write to file
 *   0x47  – get current directory
 *   0x4C  – exit with return code
 */

#include "dos.h"

/* ── DOS path → VFS path translation ─────────────────────────────────────── */
/* Converts "C:\PATH\FILE" -> "/path/file", handles backslashes, drive
 * letters, and uppercase.  The output buffer must be at least 256 bytes. */
static void dos_path_to_vfs(const char *dos, char *vfs, int vfs_max) {
    int vi = 0;
    /* Skip leading drive letter (e.g., "C:" or "c:") */
    if (dos[0] && dos[1] == ':') dos += 2;

    /* Copy and translate the rest */
    while (*dos && vi < vfs_max - 1) {
        char c = *dos++;
        if (c == '\\') c = '/';
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32); /* lowercase */
        vfs[vi++] = c;
    }
    vfs[vi] = '\0';

    /* Collapse double slashes */
    char *dst = vfs;
    for (char *src = vfs; *src; src++) {
        *dst = *src;
        if (*src == '/') {
            while (src[1] == '/') src++;
        }
        dst++;
    }
    *dst = '\0';
}

/* Memory-access helpers defined in dos_emu.c */
extern uint8_t  dos_read_seg_b(struct dos_cpu_state *, uint16_t, uint16_t);
extern uint16_t dos_read_seg_w(struct dos_cpu_state *, uint16_t, uint16_t);
extern void     dos_write_seg_b(struct dos_cpu_state *, uint16_t, uint16_t, uint8_t);
extern void     dos_write_seg_w(struct dos_cpu_state *, uint16_t, uint16_t, uint16_t);

/* ------------------------------------------------------------------ */
/* Internal file handle table                                          */
/* ------------------------------------------------------------------ */
#define DOS_MAX_HANDLES 16
#define DOS_HANDLE_START 3

struct dos_file {
    int     in_use;
    char    path[256];
    uint8_t *cache;
    uint32_t size;
    uint32_t position;
    int     mode;       /* 0=read, 1=write, 2=read+write */
};

static struct dos_file dos_handles[DOS_MAX_HANDLES];
static int dos_handles_inited = 0;

static void dos_handles_init(void)
{
    if (dos_handles_inited) return;
    __builtin_memset(dos_handles, 0, sizeof(dos_handles));
    dos_handles_inited = 1;
}

static int dos_alloc_handle(void)
{
    for (int i = DOS_HANDLE_START; i < DOS_MAX_HANDLES; i++) {
        if (!dos_handles[i].in_use) {
            __builtin_memset(&dos_handles[i], 0, sizeof(dos_handles[i]));
            dos_handles[i].in_use = 1;
            return i;
        }
    }
    return -1;
}

static void dos_free_handle(int h)
{
    if (h < DOS_HANDLE_START || h >= DOS_MAX_HANDLES) return;
    if (dos_handles[h].cache) {
        /* free not used since this is kernel code; we zero the handle */
        dos_handles[h].cache = 0;
    }
    __builtin_memset(&dos_handles[h], 0, sizeof(dos_handles[h]));
}

/* ------------------------------------------------------------------ */
/* Main INT 21h dispatcher                                             */
/* ------------------------------------------------------------------ */
void dos_handle_int21(struct dos_cpu_state *state)
{
    dos_handles_init();
    uint8_t ah = (uint8_t)(state->ax >> 8);

    switch (ah) {

    /* 0x00 – exit program */
    case 0x00:
        state->running = 0;
        break;

    /* 0x01 – read char with echo */
    case 0x01: {
        char c = keyboard_getchar();
        state->ax = (state->ax & 0xFF00) | (uint8_t)c;
        kprintf("%c", c);
        break;
    }

    /* 0x02 – print char in DL */
    case 0x02: {
        char c = (char)(state->dx & 0xFF);
        kprintf("%c", c);
        break;
    }

    /* 0x07 – read char without echo */
    case 0x07: {
        char c = keyboard_getchar();
        state->ax = (state->ax & 0xFF00) | (uint8_t)c;
        break;
    }

    /* 0x08 – read char no echo, check Ctrl-C */
    case 0x08: {
        char c = keyboard_getchar();
        state->ax = (state->ax & 0xFF00) | (uint8_t)c;
        break;
    }

    /* 0x09 – print $-terminated string at DS:DX */
    case 0x09: {
        uint16_t off = state->dx;
        while (1) {
            uint8_t c = dos_read_seg_b(state, state->ds, off);
            if (c == '$') break;
            kprintf("%c", c);
            off++;
        }
        break;
    }

    /* 0x0A – buffered input at DS:DX */
    case 0x0A: {
        uint16_t off = state->dx;
        uint8_t maxlen = dos_read_seg_b(state, state->ds, off);
        if (maxlen < 2) break;
        char buf[256];
        int pos = 0;
        while (pos < maxlen - 1) {
            char c = keyboard_getchar();
            if (c == '\r') {
                kprintf("\r\n");
                buf[pos] = '\r';
                pos++;
                break;
            }
            if (c == '\b' && pos > 0) {
                kprintf("\b \b");
                pos--;
                continue;
            }
            kprintf("%c", c);
            buf[pos] = c;
            pos++;
        }
        dos_write_seg_b(state, state->ds, off + 1, (uint8_t)pos);
        for (int i = 0; i < pos; i++)
            dos_write_seg_b(state, state->ds, off + 2 + i, (uint8_t)buf[i]);
        break;
    }

    /* 0x25 – set interrupt vector (ignored) */
    case 0x25:
        break;

    /* 0x2A – get system date */
    case 0x2A: {
        state->cx = 0x07DB; /* year = 2012 */
        state->dx = (1 << 8) | 15; /* DH = month (1), DL = day (15) */
        state->ax = (state->ax & 0xFF00) | 0; /* AL = day of week (0 = Sunday) */
        break;
    }

    /* 0x2C – get system time */
    case 0x2C: {
        state->cx = (12 << 8) | 30; /* CH = hour (12), CL = minute (30) */
        state->dx = (45 << 8) | 0;  /* DH = second (45), DL = hundredths (0) */
        break;
    }

    /* 0x30 – get DOS version */
    case 0x30: {
        state->ax = 0x0003; /* DOS 3.0 */
        state->bx = 0;      /* OEM number */
        state->cx = 0;      /* version flag */
        break;
    }

    /* 0x35 – get interrupt vector */
    case 0x35: {
        /* AL has interrupt number; return ES:BX = vector (0) */
        state->es = 0;
        state->bx = 0;
        break;
    }

    /* 0x3D – open file */
    case 0x3D: {
        uint16_t off = state->dx;
        char dos_path[256];
        int i;
        for (i = 0; i < 255; i++) {
            uint8_t c = dos_read_seg_b(state, state->ds, off + i);
            dos_path[i] = (char)c;
            if (c == 0) break;
        }
        dos_path[i] = 0;

        /* Translate DOS path to VFS path */
        char path[256];
        dos_path_to_vfs(dos_path, path, sizeof(path));

        uint8_t mode = (uint8_t)(state->ax & 0xFF);

        /* Try to read the file */
        uint32_t out_size = 0;
        struct vfs_stat st;
        if (vfs_stat(path, &st) != 0) {
            state->ax = 2; /* file not found */
            state->flags |= DOS_FLAG_CF;
            break;
        }
        uint32_t fsize = st.st_size;
        if (fsize > 65536) fsize = 65536; /* cap for emulator */
        uint8_t *fbuf = (uint8_t *)kmalloc(fsize ? fsize : 1);
        if (!fbuf) {
            state->ax = 8; /* insufficient memory */
            state->flags |= DOS_FLAG_CF;
            break;
        }
        int ret = vfs_read(path, fbuf, fsize, &out_size);
        if (ret == 0) {
            int h = dos_alloc_handle();
            if (h < 0) {
                kfree(fbuf);
                state->ax = 4; /* too many open files */
                state->flags |= DOS_FLAG_CF;
                break;
            }
            __builtin_memcpy(dos_handles[h].path, path, 256);
            dos_handles[h].size = out_size;
            dos_handles[h].mode = mode;
            dos_handles[h].position = 0;
            dos_handles[h].cache = fbuf;
            state->ax = (uint16_t)h;
            state->flags &= ~DOS_FLAG_CF;
        } else {
            kfree(fbuf);
            state->ax = 2; /* file not found */
            state->flags |= DOS_FLAG_CF;
        }
        break;
    }

    /* 0x3E – close file */
    case 0x3E: {
        uint16_t handle = state->bx;
        if (handle >= DOS_HANDLE_START && handle < DOS_MAX_HANDLES && dos_handles[handle].in_use) {
            if (dos_handles[handle].cache) {
                kfree(dos_handles[handle].cache);
                dos_handles[handle].cache = 0;
            }
            dos_free_handle(handle);
            state->flags &= ~DOS_FLAG_CF;
        } else {
            state->flags |= DOS_FLAG_CF;
            state->ax = 6; /* invalid handle */
        }
        break;
    }

    /* 0x3F – read from file */
    case 0x3F: {
        uint16_t handle = state->bx;
        uint16_t count = state->cx;
        uint16_t buf_off = state->dx;

        if (handle >= DOS_HANDLE_START && handle < DOS_MAX_HANDLES && dos_handles[handle].in_use) {
            struct dos_file *f = &dos_handles[handle];
            uint32_t avail = f->size - f->position;
            uint32_t to_read = count < avail ? count : avail;

            if (f->cache) {
                for (uint32_t i = 0; i < to_read; i++)
                    dos_write_seg_b(state, state->ds, buf_off + i, f->cache[f->position + i]);
            }
            f->position += to_read;
            state->ax = (uint16_t)to_read;
            state->flags &= ~DOS_FLAG_CF;
        } else {
            state->ax = 6; /* invalid handle */
            state->flags |= DOS_FLAG_CF;
        }
        break;
    }

    /* 0x40 – write to file */
    case 0x40: {
        uint16_t handle = state->bx;
        uint16_t count = state->cx;
        uint16_t buf_off = state->dx;

        if (handle >= DOS_HANDLE_START && handle < DOS_MAX_HANDLES && dos_handles[handle].in_use) {
            struct dos_file *f = &dos_handles[handle];
            char buf[512];
            uint32_t to_write = count < sizeof(buf) ? count : sizeof(buf);
            for (uint32_t i = 0; i < to_write; i++)
                buf[i] = (char)dos_read_seg_b(state, state->ds, buf_off + i);

            int ret = fs_append(f->path, buf, to_write);
            if (ret == 0) {
                state->ax = (uint16_t)to_write;
                state->flags &= ~DOS_FLAG_CF;
            } else {
                state->ax = 0;
                state->flags |= DOS_FLAG_CF;
            }
        } else {
            state->ax = 6;
            state->flags |= DOS_FLAG_CF;
        }
        break;
    }

    /* 0x47 – get current directory */
    case 0x47: {
        /* Return dummy current directory "C:\\" at DS:SI */
        dos_write_seg_b(state, state->ds, state->si, 'C');
        dos_write_seg_b(state, state->ds, state->si + 1, '\\');
        dos_write_seg_b(state, state->ds, state->si + 2, 0);
        state->flags &= ~DOS_FLAG_CF;
        break;
    }

    /* 0x4C – exit with return code */
    case 0x4C:
        state->running = 0;
        break;

    default:
        break;
    }
}

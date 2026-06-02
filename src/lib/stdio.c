/*
 * stdio.c — buffered stdio implementation
 *
 * Provides fopen / fclose / fread / fwrite / fprintf / fscanf and related
 * stdio functions atop the kernel's fd-based syscall interface.
 *
 * Buffering:
 *   - Read buffer: filled lazily on first fgetc/fread after buffer is empty.
 *   - Write buffer: flushed on fflush, fclose, or when full.
 *   - Default buffer size: 4096 bytes (BUFSIZ).
 *
 * Mode support:
 *   "r"  — read-only, start at beginning.
 *   "w"  — write-only, truncate to zero.
 *   "a"  — append, start at end, write-only.
 *   "r+" — read+write, start at beginning.
 *   "w+" — read+write, truncate to zero.
 *   "a+" — read+append, start at end.
 *   "b"  — binary flag (no-op, handled transparently).
 *
 * Limitations:
 *   - Maximum 32 simultaneous FILE streams.
 *   - No wide-character support.
 *   - fscanf supports only the most common format specifiers.
 */

#include "stdio.h"
#include "string.h"
#include "stdlib.h"
#include "printf.h"
#include "syscall.h"
#include "errno.h"
#include "export.h"
#include "libc.h"

/* ── Internal helpers ────────────────────────────────────────────── */

/* Maximum number of simultaneously open FILE streams */
#define STDIO_MAX_STREAMS 32

/* Statically allocated stream table (no dynamic allocation for FILE structs) */
static FILE stdio_streams[STDIO_MAX_STREAMS];
static int  stdio_initialized = 0;

/* Pre-defined standard streams (serial console via fd 0/1/2) */
FILE *stdin  = NULL;
FILE *stdout = NULL;
FILE *stderr = NULL;

/* ── Parse fopen mode string into FMODE_* bitmask ───────────────── */

static int parse_mode(const char *mode)
{
    int flags = FMODE_NONE;

    if (!mode) return flags;

    /* Check for read/write/append */
    if (mode[0] == 'r') {
        flags = FMODE_READ;
    } else if (mode[0] == 'w') {
        flags = FMODE_WRITE;
    } else if (mode[0] == 'a') {
        flags = FMODE_APPEND | FMODE_WRITE;
    } else {
        return FMODE_NONE;
    }

    /* Check for '+' modifier (read+write) */
    int i = 1;
    if (mode[i] == '+') {
        flags |= FMODE_RDWR;
        if (flags & FMODE_READ)
            flags |= FMODE_WRITE;
        else if (flags & FMODE_WRITE)
            flags |= FMODE_READ;
        else if (flags & FMODE_APPEND)
            flags |= FMODE_READ;
        i++;
    }

    /* Check for 'b' binary flag (no-op on this kernel) */
    if (mode[i] == 'b' || (mode[1] == 'b' && mode[2] == '\0')) {
        flags |= FMODE_BINARY;
    }

    return flags;
}

/* ── Flush write buffer to kernel fd ────────────────────────────── */

static int flush_wbuf(FILE *f)
{
    if (!f || !(f->flags & (FMODE_WRITE | FMODE_RDWR)))
        return 0;

    struct stdio_buf *wb = &f->wbuf;
    if (wb->len == 0)
        return 0;

    int written = libc_fd_write(f->fd, wb->base, (uint32_t)wb->len);
    if (written < 0 || written != wb->len) {
        f->error = 1;
        return EOF;
    }

    wb->len = 0;
    return 0;
}

/* ── Fill read buffer from kernel fd ────────────────────────────── */

static int fill_rbuf(FILE *f)
{
    if (!f || !(f->flags & FMODE_READ))
        return EOF;

    struct stdio_buf *rb = &f->rbuf;
    rb->len = 0;
    rb->pos = 0;

    int nread = libc_fd_read(f->fd, rb->base, (uint32_t)rb->cap);
    if (nread < 0) {
        f->error = 1;
        return EOF;
    }
    if (nread == 0) {
        f->eof = 1;
        return EOF;
    }

    rb->len = nread;
    return 0;
}

/* ── Allocate a FILE slot ───────────────────────────────────────── */

static FILE *stdio_alloc_slot(void)
{
    if (!stdio_initialized) {
        memset(stdio_streams, 0, sizeof(stdio_streams));
        stdio_initialized = 1;
    }

    for (int i = 0; i < STDIO_MAX_STREAMS; i++) {
        if (stdio_streams[i].fd == 0 && !stdio_streams[i].flags) {
            /* Check if it's genuinely free (fd 0 with no flags) */
            /* But fd 0 might be stdin — skip if it's stdin */
            if (i == 0 && stdin)
                continue;
            return &stdio_streams[i];
        }
    }
    return NULL;
}

/* ── Free a FILE slot ───────────────────────────────────────────── */

static void stdio_free_slot(FILE *f)
{
    if (!f) return;
    memset(f, 0, sizeof(*f));
}

/* ── Initialize standard streams ────────────────────────────────── */

static void stdio_init_streams(void)
{
    if (stdin) return;  /* already initialised */

    /* stdin from fd 0 */
    stdin = stdio_alloc_slot();
    if (stdin) {
        stdin->fd     = 0;
        stdin->flags  = FMODE_READ | FMODE_BINARY;
        stdin->eof    = 0;
        stdin->error  = 0;
        stdin->unget  = EOF;
        stdin->rbuf.base = (uint8_t *)libc_malloc(BUFSIZ);
        if (stdin->rbuf.base) {
            stdin->rbuf.len = 0;
            stdin->rbuf.pos = 0;
            stdin->rbuf.cap = BUFSIZ;
            stdin->rbuf.own_buf = 1;
        }
    }

    /* stdout from fd 1 */
    stdout = stdio_alloc_slot();
    if (stdout) {
        stdout->fd     = 1;
        stdout->flags  = FMODE_WRITE | FMODE_BINARY;
        stdout->eof    = 0;
        stdout->error  = 0;
        stdout->unget  = EOF;
        stdout->wbuf.base = (uint8_t *)libc_malloc(BUFSIZ);
        if (stdout->wbuf.base) {
            stdout->wbuf.len = 0;
            stdout->wbuf.pos = 0;
            stdout->wbuf.cap = BUFSIZ;
            stdout->wbuf.own_buf = 1;
        }
    }

    /* stderr from fd 2 (unbuffered: 1-byte buffer triggers immediate flush) */
    stderr = stdio_alloc_slot();
    if (stderr) {
        stderr->fd     = 2;
        stderr->flags  = FMODE_WRITE | FMODE_BINARY;
        stderr->eof    = 0;
        stderr->error  = 0;
        stderr->unget  = EOF;
        /* Use a tiny buffer so every write flushes */
        stderr->wbuf.base = (uint8_t *)libc_malloc(1);
        if (stderr->wbuf.base) {
            stderr->wbuf.len = 0;
            stderr->wbuf.pos = 0;
            stderr->wbuf.cap = 1;
            stderr->wbuf.own_buf = 1;
        }
    }
}

/* ── fopen / fclose ─────────────────────────────────────────────── */

FILE *fopen(const char *path, const char *mode)
{
    if (!path || !mode)
        return NULL;

    /* Ensure standard streams are initialised */
    if (!stdio_initialized)
        stdio_init_streams();

    int flags = parse_mode(mode);
    if (flags == FMODE_NONE)
        return NULL;

    /* Determine open flags */
    int open_flags = 0;
    int is_read  = (flags & FMODE_READ)  != 0;
    int is_write = (flags & FMODE_WRITE) != 0;
    int is_append = (flags & FMODE_APPEND) != 0;

    /* Translate to kernel open flags */
    if (is_read && !is_write) {
        open_flags = 0;  /* O_RDONLY */
    } else if (is_write && !is_read && !is_append) {
        open_flags = 0x201;  /* O_WRONLY | O_CREAT | O_TRUNC */
    } else if (is_write && is_append && !is_read) {
        open_flags = 0x2001; /* O_WRONLY | O_CREAT | O_APPEND */
    } else if (is_read && is_write && !is_append) {
        open_flags = 0x202;  /* O_RDWR | O_CREAT | O_TRUNC */
    } else if (is_read && is_write && is_append) {
        open_flags = 0x2002; /* O_RDWR | O_CREAT | O_APPEND */
    } else {
        return NULL;
    }

    /* Use the kernel's open syscall */
    int fd = (int)libc_syscall(SYS_OPEN, (uint64_t)(uintptr_t)path,
                                (uint64_t)open_flags, 0, 0, 0);
    if (fd < 0)
        return NULL;

    /* Allocate a FILE slot */
    FILE *f = stdio_alloc_slot();
    if (!f) {
        libc_syscall(SYS_CLOSE, (uint64_t)(int64_t)fd, 0, 0, 0, 0);
        return NULL;
    }

    f->fd    = fd;
    f->flags = flags;
    f->eof   = 0;
    f->error = 0;
    f->unget = EOF;

    /* Allocate read buffer if readable */
    if (flags & FMODE_READ) {
        f->rbuf.base = (uint8_t *)libc_malloc(BUFSIZ);
        if (!f->rbuf.base) {
            libc_syscall(SYS_CLOSE, (uint64_t)(int64_t)fd, 0, 0, 0, 0);
            stdio_free_slot(f);
            return NULL;
        }
        f->rbuf.len = 0;
        f->rbuf.pos = 0;
        f->rbuf.cap = BUFSIZ;
        f->rbuf.own_buf = 1;
    }

    /* Allocate write buffer if writable */
    if (flags & FMODE_WRITE) {
        f->wbuf.base = (uint8_t *)libc_malloc(BUFSIZ);
        if (!f->wbuf.base) {
            if (f->rbuf.base) libc_free(f->rbuf.base);
            libc_syscall(SYS_CLOSE, (uint64_t)(int64_t)fd, 0, 0, 0, 0);
            stdio_free_slot(f);
            return NULL;
        }
        f->wbuf.len = 0;
        f->wbuf.pos = 0;
        f->wbuf.cap = BUFSIZ;
        f->wbuf.own_buf = 1;
    }

    /* For append mode, seek to end */
    if (is_append) {
        libc_lseek(fd, 0, 2);  /* SEEK_END = 2 */
    }

    return f;
}

int fclose(FILE *stream)
{
    if (!stream) return EOF;

    int ret = 0;

    /* Flush pending writes */
    if (stream->flags & (FMODE_WRITE | FMODE_RDWR)) {
        if (flush_wbuf(stream) != 0)
            ret = EOF;
    }

    /* Free buffers */
    if (stream->rbuf.own_buf && stream->rbuf.base)
        libc_free(stream->rbuf.base);
    if (stream->wbuf.own_buf && stream->wbuf.base)
        libc_free(stream->wbuf.base);

    stream->rbuf.base = NULL;
    stream->wbuf.base = NULL;

    /* Close kernel fd (unless it's a standard stream) */
    if (stream->fd > 2) {
        libc_syscall(SYS_CLOSE, (uint64_t)(int64_t)stream->fd, 0, 0, 0, 0);
    }

    /* Free the slot (but not for standard streams) */
    if (stream != stdin && stream != stdout && stream != stderr)
        stdio_free_slot(stream);

    return ret;
}

/* ── fread / fwrite ─────────────────────────────────────────────── */

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    if (!ptr || !stream || size == 0 || nmemb == 0)
        return 0;
    if (!(stream->flags & FMODE_READ))
        return 0;

    size_t total_bytes = size * nmemb;
    size_t bytes_done = 0;
    uint8_t *dst = (uint8_t *)ptr;

    /* If the caller reads small amounts, check pushback first */
    if (stream->unget != EOF && bytes_done < total_bytes) {
        dst[bytes_done++] = (uint8_t)stream->unget;
        stream->unget = EOF;
    }

    while (bytes_done < total_bytes) {
        struct stdio_buf *rb = &stream->rbuf;

        /* Refill if buffer is empty */
        if (rb->pos >= rb->len) {
            if (fill_rbuf(stream) != 0) {
                /* EOF or error */
                break;
            }
        }

        /* Copy as much as we can from the buffer */
        int avail = rb->len - rb->pos;
        int need  = (int)(total_bytes - bytes_done);
        int copy  = (avail < need) ? avail : need;

        if (copy > 0) {
            memcpy(dst + bytes_done, rb->base + rb->pos, (size_t)copy);
            rb->pos += copy;
            bytes_done += (size_t)copy;
        }
    }

    /* Return number of elements (not bytes) */
    return bytes_done / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    if (!ptr || !stream || size == 0 || nmemb == 0)
        return 0;
    if (!(stream->flags & FMODE_WRITE))
        return 0;

    size_t total_bytes = size * nmemb;
    size_t bytes_done = 0;
    const uint8_t *src = (const uint8_t *)ptr;

    while (bytes_done < total_bytes) {
        struct stdio_buf *wb = &stream->wbuf;

        /* Flush if buffer is full */
        if (wb->len >= wb->cap) {
            if (flush_wbuf(stream) != 0)
                break;
        }

        /* Copy as much as we can into the buffer */
        int space = wb->cap - wb->len;
        int need  = (int)(total_bytes - bytes_done);
        int copy  = (space < need) ? space : need;

        if (copy > 0) {
            memcpy(wb->base + wb->len, src + bytes_done, (size_t)copy);
            wb->len += copy;
            bytes_done += (size_t)copy;
        }

        /* If the buffer is full after copy, flush */
        if (wb->len >= wb->cap) {
            if (flush_wbuf(stream) != 0)
                break;
        }
    }

    return bytes_done / size;
}

int fflush(FILE *stream)
{
    if (!stream) {
        /* fflush(NULL) — flush all streams */
        int ret = 0;
        for (int i = 0; i < STDIO_MAX_STREAMS; i++) {
            if (stdio_streams[i].flags & (FMODE_WRITE | FMODE_RDWR)) {
                if (flush_wbuf(&stdio_streams[i]) != 0)
                    ret = EOF;
            }
        }
        return ret;
    }

    if (!(stream->flags & (FMODE_WRITE | FMODE_RDWR)))
        return 0;

    return flush_wbuf(stream);
}

/* ── Character I/O ──────────────────────────────────────────────── */

int fgetc(FILE *stream)
{
    if (!stream) return EOF;

    /* Check pushback first */
    if (stream->unget != EOF) {
        int c = stream->unget;
        stream->unget = EOF;
        return c;
    }

    unsigned char ch;
    size_t n = fread(&ch, 1, 1, stream);
    if (n == 0) return EOF;
    return (int)ch;
}

int fputc(int c, FILE *stream)
{
    if (!stream) return EOF;
    unsigned char ch = (unsigned char)c;
    size_t n = fwrite(&ch, 1, 1, stream);
    if (n == 0) return EOF;
    return (int)ch;
}

int ungetc(int c, FILE *stream)
{
    if (!stream || c == EOF) return EOF;
    stream->unget = c;
    stream->eof = 0;  /* clear EOF on pushback */
    return c;
}

char *fgets(char *s, int size, FILE *stream)
{
    if (!s || size <= 0 || !stream)
        return NULL;

    int i = 0;
    while (i < size - 1) {
        int c = fgetc(stream);
        if (c == EOF) {
            if (i == 0) return NULL;  /* no bytes read */
            break;
        }
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    s[i] = '\0';
    return s;
}

/* ── Seek / Tell ────────────────────────────────────────────────── */

int fseek(FILE *stream, long offset, int whence)
{
    if (!stream) return -1;

    /* Flush write buffer before seeking */
    if (stream->flags & (FMODE_WRITE | FMODE_RDWR)) {
        if (flush_wbuf(stream) != 0)
            return -1;
    }

    /* Discard read buffer (data may be stale after seek) */
    stream->rbuf.len = 0;
    stream->rbuf.pos = 0;
    stream->unget = EOF;
    stream->eof = 0;

    int64_t result = libc_lseek(stream->fd, (int64_t)offset, whence);
    if (result < 0)
        return -1;

    return 0;
}

long ftell(FILE *stream)
{
    if (!stream) return -1L;
    int64_t result = libc_lseek(stream->fd, 0, 1);  /* SEEK_CUR = 1 */
    if (result < 0) return -1L;

    /* Adjust for read buffer offset (data read from kernel but not consumed) */
    if (stream->flags & FMODE_READ) {
        result -= (int64_t)(stream->rbuf.len - stream->rbuf.pos);
    }

    /* Adjust for write buffer (data pending flush) */
    if (stream->flags & FMODE_WRITE) {
        result += (int64_t)stream->wbuf.len;
    }

    return (long)result;
}

/* ── Status ──────────────────────────────────────────────────────── */

int feof(FILE *stream)
{
    return stream ? stream->eof : 0;
}

int ferror(FILE *stream)
{
    return stream ? stream->error : 0;
}

void clearerr(FILE *stream)
{
    if (stream) {
        stream->eof   = 0;
        stream->error = 0;
    }
}

/* ── vfprintf — formatted output ─────────────────────────────────── */

/*
 * fprintf / vfprintf use snprintf to format into a stack buffer, then
 * write the result via fwrite.  This reuses the kernel's full format
 * engine (already in printf.c) without needing a callback variant.
 *
 * For very long output (>4KB), we fall back to iterative formatting
 * in 4KB chunks.
 */

#define FPRINTF_BUF_SIZE 4096

int vfprintf(FILE *stream, const char *fmt, __builtin_va_list ap)
{
    if (!stream || !fmt) return 0;

    char buf[FPRINTF_BUF_SIZE];
    int len = vsnprintf(buf, sizeof(buf), fmt, ap);

    if (len < 0)
        return 0;

    if ((size_t)len < sizeof(buf)) {
        /* Fit entirely in the stack buffer */
        size_t written = fwrite(buf, 1, (size_t)len, stream);
        return (int)written;
    }

    /* Formatting was truncated by buffer size — write what we have */
    fwrite(buf, 1, sizeof(buf) - 1, stream);
    return (int)sizeof(buf) - 1;
}

int fprintf(FILE *stream, const char *fmt, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int ret = vfprintf(stream, fmt, ap);
    __builtin_va_end(ap);
    return ret;
}

/* ── vfscanf — formatted input ──────────────────────────────────── */

/*
 * Minimal fscanf implementation.  Supports:
 *   - %d, %i, %u — decimal integer
 *   - %x, %X     — hexadecimal integer
 *   - %o         — octal integer
 *   - %s         — whitespace-delimited string
 *   - %c         — single character (no skip whitespace)
 *   - %f         — floating-point (as double; simple parser)
 *   - %[set]     — scanset
 *   - %%         — literal '%'
 *   - *          — assignment suppression
 *   - width      — maximum field width
 *
 * Returns number of successful assignments, or EOF on input failure before
 * any conversion.
 */

/* Skip whitespace characters */
static void skip_whitespace(FILE *stream)
{
    while (!feof(stream)) {
        int c = fgetc(stream);
        if (c == EOF) return;
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\v' && c != '\f') {
            ungetc(c, stream);
            return;
        }
    }
}

/* Read an integer in the given base */
static unsigned long long read_int(FILE *stream, int base, int *chars_read,
                                    int *overflow)
{
    unsigned long long val = 0;
    int count = 0;
    *overflow = 0;

    /* Skip leading whitespace */
    skip_whitespace(stream);

    /* Handle sign */
    int sign = 1;
    int c = fgetc(stream);
    if (c == '-') { sign = -1; count++; }
    else if (c == '+') { count++; }
    else if (c != EOF) { ungetc(c, stream); }
    else return 0;

    /* Read digits */
    int first_non_zero = 0;
    while (!feof(stream)) {
        c = fgetc(stream);
        if (c == EOF) break;

        int digit = -1;
        if (c >= '0' && c <= '9') digit = c - '0';
        else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;

        if (digit < 0 || digit >= base) {
            if (c != EOF) ungetc(c, stream);
            break;
        }
        if (digit > 0) first_non_zero = 1;

        if (!*overflow) {
            unsigned long long prev = val;
            val = val * (unsigned long long)base + (unsigned long long)digit;

            /* Detect overflow */
            if (val / (unsigned long long)base != prev && first_non_zero) {
                *overflow = 1;
                val = (unsigned long long)-1;
            }
        }
        count++;
    }

    *chars_read = count;
    return sign > 0 ? val : (unsigned long long)(-(long long)val);
}

int vfscanf(FILE *stream, const char *fmt, __builtin_va_list ap)
{
    if (!stream || !fmt)
        return EOF;

    int assignments = 0;

    while (*fmt) {
        /* Skip whitespace in format string */
        if (*fmt == ' ' || *fmt == '\t' || *fmt == '\n') {
            /* Whitespace in format skips any amount of whitespace in input */
            skip_whitespace(stream);
            fmt++;
            continue;
        }

        /* Non-format characters must match literally */
        if (*fmt != '%') {
            int c = fgetc(stream);
            if (c == EOF) {
                if (assignments == 0) return EOF;
                break;
            }
            if ((unsigned char)c != (unsigned char)*fmt) {
                /* Mismatch — push back and stop */
                ungetc(c, stream);
                break;
            }
            fmt++;
            continue;
        }

        /* ── Parse % format specifier ── */
        fmt++;  /* skip '%' */

        /* Check for %% */
        if (*fmt == '%') {
            int c = fgetc(stream);
            if (c == EOF) { if (assignments == 0) return EOF; break; }
            if (c != '%') { ungetc(c, stream); break; }
            fmt++;
            continue;
        }

        /* Assignment suppression '*' */
        int suppress = 0;
        if (*fmt == '*') {
            suppress = 1;
            fmt++;
        }

        /* Field width (optional) */
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt - '0');
            fmt++;
        }

        /* Length modifier (ignored for our simple implementation) */
        int long_long = 0;
        while (*fmt == 'h' || *fmt == 'l' || *fmt == 'L' ||
               *fmt == 'z' || *fmt == 't' || *fmt == 'j') {
            if (*fmt == 'l') long_long++;
            if (*fmt == 'L') long_long = 2;
            fmt++;
        }
        if (long_long > 2) long_long = 2;

        /* Conversion specifier */
        char spec = *fmt;
        if (!spec) break;
        fmt++;

        switch (spec) {
        case 'd':
        case 'i': {
            /* Signed decimal */
            int chars;
            int overflow;
            unsigned long long raw_val = read_int(stream, 10, &chars, &overflow);
            if (chars == 0) {
                if (assignments == 0) return EOF;
                goto done;
            }
            if (!suppress) {
                int *p = __builtin_va_arg(ap, int *);
                *p = (int)raw_val;
                assignments++;
            }
            break;
        }

        case 'u': {
            int chars;
            int overflow;
            unsigned long long raw_val = read_int(stream, 10, &chars, &overflow);
            if (chars == 0) {
                if (assignments == 0) return EOF;
                goto done;
            }
            if (!suppress) {
                unsigned int *p = __builtin_va_arg(ap, unsigned int *);
                *p = (unsigned int)raw_val;
                assignments++;
            }
            break;
        }

        case 'x':
        case 'X': {
            int chars;
            int overflow;
            unsigned long long raw_val = read_int(stream, 16, &chars, &overflow);
            if (chars == 0) {
                if (assignments == 0) return EOF;
                goto done;
            }
            if (!suppress) {
                unsigned int *p = __builtin_va_arg(ap, unsigned int *);
                *p = (unsigned int)raw_val;
                assignments++;
            }
            break;
        }

        case 'o': {
            int chars;
            int overflow;
            unsigned long long raw_val = read_int(stream, 8, &chars, &overflow);
            if (chars == 0) {
                if (assignments == 0) return EOF;
                goto done;
            }
            if (!suppress) {
                unsigned int *p = __builtin_va_arg(ap, unsigned int *);
                *p = (unsigned int)raw_val;
                assignments++;
            }
            break;
        }

        case 's': {
            /* String (whitespace-delimited) */
            skip_whitespace(stream);
            if (feof(stream)) {
                if (assignments == 0) return EOF;
                goto done;
            }
            char *buf = NULL;
            if (!suppress) {
                buf = __builtin_va_arg(ap, char *);
            }
            int pos = 0;
            while (!feof(stream)) {
                int c = fgetc(stream);
                if (c == EOF || c == ' ' || c == '\t' || c == '\n') {
                    if (c != EOF) ungetc(c, stream);
                    break;
                }
                if (!suppress && (width == 0 || pos < width - 1)) {
                    buf[pos++] = (char)c;
                }
            }
            if (!suppress) {
                if (buf) buf[pos] = '\0';
                assignments++;
            }
            break;
        }

        case 'c': {
            /* Single character (does NOT skip whitespace) */
            int c = fgetc(stream);
            if (c == EOF) {
                if (assignments == 0) return EOF;
                goto done;
            }
            if (!suppress) {
                char *p = __builtin_va_arg(ap, char *);
                *p = (char)c;
                assignments++;
            }
            break;
        }

        case '[': {
            /* Scanset */
            int negate = 0;
            if (*fmt == '^') { negate = 1; fmt++; }
            /* Collect the set */
            unsigned char set[32]; /* bitset for 256 chars */
            memset(set, 0, sizeof(set));
            if (*fmt == ']') { set[0] = 1; fmt++; }
            while (*fmt && *fmt != ']') {
                set[(unsigned char)*fmt / 8] |= (1 << ((unsigned char)*fmt % 8));
                fmt++;
            }
            if (*fmt == ']') fmt++;  /* skip closing ] */

            skip_whitespace(stream);
            if (feof(stream)) {
                if (assignments == 0) return EOF;
                goto done;
            }
            char *buf = NULL;
            if (!suppress) {
                buf = __builtin_va_arg(ap, char *);
            }
            int pos = 0;
            while (!feof(stream)) {
                int c = fgetc(stream);
                if (c == EOF) break;

                int in_set = (set[(unsigned char)c / 8] >> ((unsigned char)c % 8)) & 1;
                if (negate) in_set = !in_set;

                if (!in_set) {
                    if (c != EOF) ungetc(c, stream);
                    break;
                }
                if (!suppress && (width == 0 || pos < width - 1)) {
                    buf[pos++] = (char)c;
                }
            }
            if (!suppress) {
                if (buf) buf[pos] = '\0';
                assignments++;
            }
            break;
        }

        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G': {
            /* Floating point (simplified) */
            skip_whitespace(stream);

            char num_buf[64];
            int pos = 0;

            int c = fgetc(stream);
            if (c == EOF) {
                if (assignments == 0) return EOF;
                goto done;
            }
            if (c == '-' || c == '+') {
                num_buf[pos++] = (char)c;
                c = fgetc(stream);
            }
            if (c < '0' || c > '9') {
                ungetc(c, stream);
                if (assignments == 0) return EOF;
                goto done;
            }
            while (pos < (int)sizeof(num_buf) - 1 && c != EOF) {
                if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E') {
                    num_buf[pos++] = (char)c;
                    c = fgetc(stream);
                    if ((c == '+' || c == '-') && pos > 0 &&
                        (num_buf[pos-1] == 'e' || num_buf[pos-1] == 'E')) {
                        continue;
                    }
                } else {
                    break;
                }
            }
            if (c != EOF) ungetc(c, stream);
            num_buf[pos] = '\0';

            if (pos == 0 || (pos == 1 && (num_buf[0] == '-' || num_buf[0] == '+'))) {
                if (assignments == 0) return EOF;
                goto done;
            }

            if (!suppress) {
                float *p = __builtin_va_arg(ap, float *);
                const char *np = num_buf;
                int neg = 0;
                if (*np == '-') { neg = 1; np++; }
                else if (*np == '+') np++;

                double int_part = 0.0;
                while (*np >= '0' && *np <= '9') {
                    int_part = int_part * 10.0 + (double)(*np - '0');
                    np++;
                }
                if (*np == '.') {
                    np++;
                    double frac_part = 0.0;
                    double divisor = 1.0;
                    while (*np >= '0' && *np <= '9') {
                        frac_part = frac_part * 10.0 + (double)(*np - '0');
                        divisor *= 10.0;
                        np++;
                    }
                    int_part += frac_part / divisor;
                }
                if (*np == 'e' || *np == 'E') {
                    np++;
                    int exp_neg = 0;
                    if (*np == '-') { exp_neg = 1; np++; }
                    else if (*np == '+') np++;
                    int exp_val = 0;
                    while (*np >= '0' && *np <= '9') {
                        exp_val = exp_val * 10 + (*np - '0');
                        np++;
                    }
                    if (exp_neg) {
                        for (int i = 0; i < exp_val; i++) int_part /= 10.0;
                    } else {
                        for (int i = 0; i < exp_val; i++) int_part *= 10.0;
                    }
                }
                if (neg) int_part = -int_part;
                *p = (float)int_part;
                assignments++;
            }
            break;
        }

        default:
            /* Unknown specifier — skip */
            break;
        }
    }

done:
    return assignments;
}

int fscanf(FILE *stream, const char *fmt, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    int ret = vfscanf(stream, fmt, ap);
    __builtin_va_end(ap);
    return ret;
}

/* ── Library init ────────────────────────────────────────────────── */

void stdio_init(void)
{
    if (stdio_initialized) return;
    stdio_init_streams();
    kprintf("[OK] stdio: buffered I/O library initialized\\n");
}

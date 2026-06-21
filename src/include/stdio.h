#ifndef STDIO_H
#define STDIO_H

#include "types.h"

/*
 * ── Buffered stdio — fopen / fread / fwrite / fclose / fprintf / fscanf ──
 *
 * Provides standard C buffered file I/O atop the kernel's fd-based syscalls.
 * Each FILE has an internal buffer (default 4 KB) and tracks read/write mode,
 * error status, and EOF flag.
 *
 * FILE limit: 32 simultaneously open streams (matches POSIX typical minimum).
 */

#define EOF             (-1)
#define BUFSIZ          4096

/* Mode flags for fopen */
enum fmode {
    FMODE_NONE   = 0,
    FMODE_READ   = (1 << 0),  /* "r"  — read-only          */
    FMODE_WRITE  = (1 << 1),  /* "w"  — write (truncate)   */
    FMODE_APPEND = (1 << 2),  /* "a"  — append              */
    FMODE_RDWR   = (1 << 3),  /* "r+" / "w+" / "a+"       */
    FMODE_BINARY = (1 << 4),  /* "b" — binary (no-op on this kernel) */
};

/* Internal buffer state */
struct stdio_buf {
    uint8_t *base;       /* malloc'd buffer base          */
    int      len;        /* number of valid bytes in buf  */
    int      pos;        /* current read position in buf  */
    int      cap;        /* allocated capacity             */
    int      own_buf;    /* 1 = we own the buffer (need free) */
};

/* The FILE structure */
typedef struct _FILE {
    int      fd;          /* kernel file descriptor (filled by SYS_OPEN) */
    int      flags;       /* FMODE_* bitmask                              */
    int      eof;         /* 1 = EOF has been reached                     */
    int      error;       /* non-zero if an I/O error occurred            */
    int      unget;       /* single-char pushback buffer (EOF=none)       */

    /* Buffering state */
    struct stdio_buf rbuf;  /* read buffer  */
    struct stdio_buf wbuf;  /* write buffer */
} FILE;

/* ── Open/Close ──────────────────────────────────────────────────── */

/* Open a file.  mode: "r", "w", "a", "r+", "w+", "a+", with optional "b". */
FILE *fopen(const char *path, const char *mode);

/* Close an open file.  Flushes pending writes first.  Returns 0 on success. */
int   fclose(FILE *stream);

/* ── Read/Write ──────────────────────────────────────────────────── */

/* Buffered read.  Returns number of items read (may be < nmemb on EOF/error). */
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);

/* Buffered write.  Returns number of items written (may be < nmemb on error). */
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);

/* Flush the write buffer to the kernel.  Returns 0 on success, EOF on error. */
int    fflush(FILE *stream);

/* ── Character I/O ───────────────────────────────────────────────── */

int  fgetc(FILE *stream);
int  fputc(int c, FILE *stream);
int  ungetc(int c, FILE *stream);
char *fgets(char *s, int size, FILE *stream);

/* ── Position ────────────────────────────────────────────────────── */

int fseek(FILE *stream, long offset, int whence);
long ftell(FILE *stream);

/* ── Status ──────────────────────────────────────────────────────── */

int feof(FILE *stream);
int ferror(FILE *stream);
void clearerr(FILE *stream);

/* ── Formatted I/O ──────────────────────────────────────────────── */

int fprintf(FILE *stream, const char *fmt, ...);
int fscanf(FILE *stream, const char *fmt, ...);
int sscanf(const char *str, const char *fmt, ...);
int vfprintf(FILE *stream, const char *fmt, __builtin_va_list ap);
int vfscanf(FILE *stream, const char *fmt, __builtin_va_list ap);

/* ── Pre-defined standard streams (terminal input/output via serial) ── */

/* Note: These are "virtual" streams that read from / write to the serial
 * console.  They share fd 0/1/2 but use buffering internally.
 * stdin/stdout/stderr are pre-initialised. */
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

#endif /* STDIO_H */

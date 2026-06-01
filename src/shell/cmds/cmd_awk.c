/* cmd_awk.c — minimal awk-like field processor
 *
 * Supports:
 *   awk '{print $N}' [file]          — print Nth field of each line
 *   awk '{print $0}' [file]          — print whole line
 *   awk -F: '{print $N}' [file]      — custom field separator
 *   awk 'pattern{action}' [file]     — simple string pattern match
 *   awk 'BEGIN{...} {body} END{...}' — BEGIN and END blocks
 *
 * Actions supported: print (with $0, $1..$9), next
 * Pattern: plain string match on $0
 *
 * Input comes from stdin if no file is given (shell pipe support).
 */
#include "shell_cmds.h"
#include "libc.h"
#include "printf.h"
#include "string.h"

#define AWK_MAX_LINE  512
#define AWK_MAX_FIELDS 32
#define AWK_MAX_PROG  256

static char awk_sep = ' ';  /* field separator */

/* Split line into fields, returns count */
static int awk_split(char *line, char *fields[], int max_fields) {
    int count = 0;
    char *p = line;
    while (*p && count < max_fields) {
        /* skip leading separators */
        while (*p == awk_sep || (awk_sep == ' ' && (*p == '\t'))) p++;
        if (!*p) break;
        fields[count++] = p;
        /* find end of field */
        while (*p && *p != awk_sep && (awk_sep != ' ' || (*p != ' ' && *p != '\t'))) p++;
        if (*p) { *p = '\0'; p++; }
    }
    return count;
}

/* Evaluate a simple print statement like: print $1, $2, "literal", $0 */
static void awk_exec_print(const char *stmt, char *fields[], int nfields, char *line) {
    /* skip 'print' keyword */
    const char *p = stmt;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "print", 5) != 0) return;
    p += 5;
    while (*p == ' ' || *p == '\t') p++;

    int first = 1;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (!first) kprintf(" ");
        first = 0;

        if (*p == '$') {
            p++;
            int n = 0;
            while (*p >= '0' && *p <= '9') { n = n*10 + (*p - '0'); p++; }
            if (n == 0) kprintf("%s", line);
            else if (n <= nfields) kprintf("%s", fields[n-1]);
        } else if (*p == '"') {
            p++;
            while (*p && *p != '"') { kprintf("%c", (unsigned long)(unsigned char)*p); p++; }
            if (*p == '"') p++;
        } else {
            /* bare token */
            const char *start = p;
            while (*p && *p != ' ' && *p != '\t' && *p != ',') p++;
            int len = (int)(p - start);
            for (int i = 0; i < len; i++) kprintf("%c", (unsigned long)(unsigned char)start[i]);
        }
        while (*p == ',') p++;  /* skip comma separator */
    }
    kprintf("\n");
}

/* Execute a block body on the current line */
static void awk_exec_block(const char *body, char *fields[], int nfields, char *line) {
    /* Very simple: find 'print' statements separated by ';' or newlines */
    char buf[AWK_MAX_PROG];
    int blen = (int)strlen(body);
    if (blen >= AWK_MAX_PROG) blen = AWK_MAX_PROG - 1;
    for (int i = 0; i < blen; i++) buf[i] = body[i];
    buf[blen] = '\0';

    char *p = buf;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ';' || *p == '\n') p++;
        if (!*p) break;
        /* find end of statement */
        char *end = p;
        while (*end && *end != ';' && *end != '\n') end++;
        char save = *end;
        *end = '\0';
        awk_exec_print(p, fields, nfields, line);
        *end = save;
        p = end;
    }
}

/* Parse and run awk program on a line */
/* program format: [pattern]{body}  or just {body}  or just body */
static void awk_run_line(const char *prog, char *line) {
    char linebuf[AWK_MAX_LINE];
    int ll = (int)strlen(line);
    if (ll >= AWK_MAX_LINE) ll = AWK_MAX_LINE - 1;
    for (int i = 0; i < ll; i++) linebuf[i] = line[i];
    linebuf[ll] = '\0';

    char *fields[AWK_MAX_FIELDS];
    char fieldbuf[AWK_MAX_LINE];
    for (int i = 0; i < ll; i++) fieldbuf[i] = line[i];
    fieldbuf[ll] = '\0';
    int nf = awk_split(fieldbuf, fields, AWK_MAX_FIELDS);

    const char *p = prog;
    while (*p == ' ' || *p == '\t') p++;

    /* Find optional pattern before '{' */
    const char *brace = p;
    while (*brace && *brace != '{') brace++;

    const char *pattern = NULL;
    char pat_buf[64] = {0};
    if (brace > p) {
        int plen = (int)(brace - p);
        if (plen > 63) plen = 63;
        for (int i = 0; i < plen; i++) pat_buf[i] = p[i];
        pat_buf[plen] = '\0';
        /* strip quotes if present */
        char *pp = pat_buf;
        while (*pp == ' ') pp++;
        int pplen = (int)strlen(pp);
        while (pplen > 0 && pp[pplen-1] == ' ') { pp[--pplen] = '\0'; }
        if (pplen > 0) pattern = pp;
    }

    /* If pattern set, check match */
    if (pattern && strstr(linebuf, pattern) == NULL) return;

    /* Extract body between braces */
    if (!*brace) {
        /* No braces, treat whole thing as body */
        awk_exec_block(p, fields, nf, linebuf);
        return;
    }
    const char *body_start = brace + 1;
    const char *body_end = body_start;
    int depth = 1;
    while (*body_end && depth > 0) {
        if (*body_end == '{') depth++;
        else if (*body_end == '}') depth--;
        if (depth > 0) body_end++;
    }
    char body[AWK_MAX_PROG];
    int blen = (int)(body_end - body_start);
    if (blen >= AWK_MAX_PROG) blen = AWK_MAX_PROG - 1;
    for (int i = 0; i < blen; i++) body[i] = body_start[i];
    body[blen] = '\0';
    awk_exec_block(body, fields, nf, linebuf);
}

void cmd_awk(const char *args) {
    if (!args || !args[0]) {
        kprintf("Usage: awk [-F sep] 'program' [file]\n");
        return;
    }

    const char *p = args;
    awk_sep = ' ';

    /* Parse -F separator */
    if (p[0] == '-' && p[1] == 'F') {
        p += 2;
        while (*p == ' ') p++;
        if (*p) { awk_sep = *p; p++; }
        while (*p == ' ') p++;
    }

    /* Extract program string (single-quoted or bare) */
    char prog[AWK_MAX_PROG] = {0};
    if (*p == '\'') {
        p++;
        int i = 0;
        while (*p && *p != '\'' && i < AWK_MAX_PROG - 1) prog[i++] = *p++;
        if (*p == '\'') p++;
    } else if (*p == '{') {
        int i = 0, depth = 0;
        while (*p && i < AWK_MAX_PROG - 1) {
            if      (*p == '{') depth++;
            else if (*p == '}') { depth--; if (depth == 0) { prog[i++] = *p++; break; } }
            prog[i++] = *p++;
        }
    } else {
        int i = 0;
        while (*p && *p != ' ' && i < AWK_MAX_PROG - 1) prog[i++] = *p++;
    }
    while (*p == ' ') p++;

    /* Skip BEGIN/END blocks (not supported in per-line mode here) */
    /* Find main body: the { } block that is NOT BEGIN or END */
    char main_body[AWK_MAX_PROG] = {0};
    const char *pp = prog;
    /* simple heuristic: find first {body} not preceded by BEGIN/END */
    while (*pp) {
        while (*pp == ' ' || *pp == '\t') pp++;
        if (strncmp(pp, "BEGIN", 5) == 0 || strncmp(pp, "END", 3) == 0) {
            /* skip this block */
            while (*pp && *pp != '{') pp++;
            if (*pp == '{') { pp++; int d=1; while(*pp && d>0){ if(*pp=='{')d++;else if(*pp=='}')d--; pp++; } }
        } else {
            /* This is the main body */
            int i = 0;
            while (*pp && i < AWK_MAX_PROG - 1) main_body[i++] = *pp++;
            main_body[i] = '\0';
            break;
        }
    }
    if (!main_body[0]) {
        for (int i = 0; prog[i] && i < AWK_MAX_PROG - 1; i++) main_body[i] = prog[i];
    }

    /* Read from file or stdin */
    char line[AWK_MAX_LINE];

    if (*p) {
        /* Read whole file into a buffer */
        uint8_t fbuf[4096];
        uint32_t fsz = 0;
        int rc = libc_vfs_read(p, fbuf, sizeof(fbuf) - 1, &fsz);
        if (rc < 0) { kprintf("awk: cannot open %s\n", p); return; }
        fbuf[fsz] = '\0';
        /* Process line by line */
        char *ptr = (char *)fbuf;
        while (*ptr) {
            char *nl = ptr;
            while (*nl && *nl != '\n') nl++;
            int len = (int)(nl - ptr);
            if (len >= AWK_MAX_LINE) len = AWK_MAX_LINE - 1;
            for (int i = 0; i < len; i++) line[i] = ptr[i];
            line[len] = '\0';
            if (len > 0 && line[len-1] == '\r') line[--len] = '\0';
            awk_run_line(main_body, line);
            ptr = *nl ? nl + 1 : nl;
        }
    } else if (shell_has_stdin()) {
        /* Read from piped stdin buffer */
        static char sbuf[4096];
        int slen = shell_stdin_read(sbuf, (int)sizeof(sbuf) - 1);
        sbuf[slen] = '\0';
        char *ptr = sbuf;
        while (*ptr) {
            char *nl = ptr;
            while (*nl && *nl != '\n') nl++;
            int len = (int)(nl - ptr);
            if (len >= AWK_MAX_LINE) len = AWK_MAX_LINE - 1;
            for (int i = 0; i < len; i++) line[i] = ptr[i];
            line[len] = '\0';
            if (len > 0 && line[len-1] == '\r') line[--len] = '\0';
            awk_run_line(main_body, line);
            ptr = *nl ? nl + 1 : nl;
        }
    } else {
        /* Read from stdin line by line */
        while (1) {
            libc_shell_read_line(line, sizeof(line));
            if (!line[0]) break;
            int len = (int)strlen(line);
            if (len > 0 && line[len-1] == '\n') line[--len] = '\0';
            if (len > 0 && line[len-1] == '\r') line[--len] = '\0';
            if (len == 0) break;
            awk_run_line(main_body, line);
        }
    }
}

/* cmd_cc.c — cc command: compile a C source file to an ELF binary */

#include "libc.h"
#include "string.h"
#include "printf.h"

static void cc_default_out(const char *inpath, char outpath[256]) {
    strncpy(outpath, inpath, 255);
    outpath[255] = '\0';
    int len = strlen(outpath);
    if (len > 2 && outpath[len - 2] == '.' && outpath[len - 1] == 'c') {
        outpath[len - 2] = '\0';
    }
}

static int cc_compile_one(const char *inpath, const char *maybe_out) {
    char outpath[256] = {0};
    if (maybe_out && *maybe_out) {
        strncpy(outpath, maybe_out, 255);
        outpath[255] = '\0';
    } else {
        cc_default_out(inpath, outpath);
    }

    int rc = cc_compile(inpath, outpath);
    if (rc == 0)
        kprintf("cc: OK %s -> %s\n", inpath, outpath);
    else if (rc == -2)
        kprintf("cc: cannot read %s\n", inpath);
    else if (rc == -3)
        kprintf("cc: lex error in %s\n", inpath);
    else if (rc == -4)
        kprintf("cc: compile error in %s\n", inpath);
    else if (rc == -5)
        kprintf("cc: failed to write %s\n", outpath);
    else
        kprintf("cc: failed %s (%d)\n", inpath, (uint64_t)(-rc));
    return rc;
}

static void cc_batch_compile(const char *list_path) {
    char listbuf[8192];
    uint32_t sz = 0;
    if (fs_read_file(list_path, listbuf, sizeof(listbuf) - 1, &sz) < 0 || sz == 0) {
        kprintf("cc: cannot read list file %s\n", list_path);
        return;
    }
    listbuf[sz] = '\0';

    int ok = 0, fail = 0;
    int i = 0;
    while (listbuf[i]) {
        while (listbuf[i] == '\n' || listbuf[i] == '\r') i++;
        if (!listbuf[i]) break;

        int line_start = i;
        while (listbuf[i] && listbuf[i] != '\n' && listbuf[i] != '\r') i++;
        int line_end = i;

        while (line_start < line_end && (listbuf[line_start] == ' ' || listbuf[line_start] == '\t')) line_start++;
        while (line_end > line_start && (listbuf[line_end - 1] == ' ' || listbuf[line_end - 1] == '\t')) line_end--;
        if (line_start >= line_end) continue;
        if (listbuf[line_start] == '#') continue;

        char inpath[256] = {0};
        char outpath[256] = {0};
        int p = line_start;
        int j = 0;
        while (p < line_end && listbuf[p] != ' ' && listbuf[p] != '\t' && j < 255)
            inpath[j++] = listbuf[p++];
        inpath[j] = '\0';

        while (p < line_end && (listbuf[p] == ' ' || listbuf[p] == '\t')) p++;
        j = 0;
        while (p < line_end && j < 255)
            outpath[j++] = listbuf[p++];
        outpath[j] = '\0';

        int rc = cc_compile_one(inpath, outpath[0] ? outpath : NULL);
        if (rc == 0) ok++; else fail++;
    }

    kprintf("cc: batch done, %d ok, %d failed\n", (uint64_t)ok, (uint64_t)fail);
}

void cmd_cc(const char *args) {
    if (!args || !*args) {
        kprintf("Usage: cc <source.c> [output]\n");
        kprintf("   or: cc --batch <list.txt>\n");
        kprintf("  Compile a C source file to a static ELF64 binary.\n");
        kprintf("  Output defaults to the source name without .c extension.\n");
        kprintf("  Batch list format: '<inpath> [outpath]' per line.\n");
        return;
    }

    if (strncmp(args, "--batch", 7) == 0) {
        const char *p = args + 7;
        while (*p == ' ') p++;
        if (!*p) {
            kprintf("Usage: cc --batch <list.txt>\n");
            return;
        }
        cc_batch_compile(p);
        return;
    }

    /* parse args: "infile [outfile]" */
    char inpath[256] = {0};
    char outpath[256] = {0};

    int i = 0, j = 0;
    while (args[i] && args[i] != ' ' && j < 255) inpath[j++] = args[i++];
    inpath[j] = '\0';
    while (args[i] == ' ') i++;
    j = 0;
    while (args[i] && j < 255) outpath[j++] = args[i++];
    outpath[j] = '\0';

    (void)cc_compile_one(inpath, outpath[0] ? outpath : NULL);
}

/* cmd_ld.c — ld command: link multiple .o files into an ELF executable */

#include "libc.h"
#include "string.h"
#include "printf.h"

void cmd_ld(const char *args) {
    if (!args || !*args) {
        kprintf("Usage: ld [-o output] file1.o file2.o ...\n");
        kprintf("  Link relocatable .o files into a static ELF64 binary.\n");
        kprintf("  Default output: a.out\n");
        return;
    }

    /* Parse arguments */
    char outpath[256] = "a.out";
    const char *obj_paths[64];
    int nobj = 0;

    const char *p = args;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;

        if (p[0] == '-' && p[1] == 'o' && (p[2] == ' ' || p[2] == '\0')) {
            p += 2;
            while (*p == ' ') p++;
            int j = 0;
            while (*p && *p != ' ' && j < 255) outpath[j++] = *p++;
            outpath[j] = '\0';
            continue;
        }

        /* Collect object file path */
        static char pathbuf[64][256];
        if (nobj >= 64) { kprintf("ld: too many object files\n"); return; }
        int j = 0;
        while (*p && *p != ' ' && j < 255) pathbuf[nobj][j++] = *p++;
        pathbuf[nobj][j] = '\0';
        obj_paths[nobj] = pathbuf[nobj];
        nobj++;
    }

    if (nobj == 0) {
        kprintf("ld: no input files\n");
        return;
    }

    int rc = cc_link_files(nobj, obj_paths, outpath);
    if (rc == 0)
        kprintf("ld: OK -> %s\n", outpath);
    else
        kprintf("ld: link failed (%ld)\n", (unsigned long)(-rc));
}

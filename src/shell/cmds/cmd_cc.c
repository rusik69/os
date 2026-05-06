/* cmd_cc.c — cc command: compile a C source file to an ELF binary */

#include "libc.h"
#include "string.h"
#include "printf.h"

void cmd_cc(const char *args) {
    if (!args || !*args) {
        kprintf("Usage: cc <source.c> [output]\n");
        kprintf("  Compile a C source file to a static ELF64 binary.\n");
        kprintf("  Output defaults to the source name without .c extension.\n");
        return;
    }

    /* parse args: "infile [outfile]" */
    char inpath[64] = {0};
    char outpath[64] = {0};

    int i = 0, j = 0;
    while (args[i] && args[i] != ' ' && j < 63) inpath[j++] = args[i++];
    inpath[j] = '\0';
    while (args[i] == ' ') i++;
    j = 0;
    while (args[i] && j < 63) outpath[j++] = args[i++];
    outpath[j] = '\0';

    /* default output: strip .c suffix */
    if (outpath[0] == '\0') {
        strncpy(outpath, inpath, 63);
        int len = strlen(outpath);
        if (len > 2 && outpath[len-2] == '.' && outpath[len-1] == 'c') {
            outpath[len-2] = '\0';
        }
    }

    int rc = cc_compile(inpath, outpath);
    if (rc == 0)
        kprintf("cc: OK -> %s\n", outpath);
    else if (rc == -2)
        kprintf("cc: cannot read %s\n", inpath);
    else if (rc == -3)
        kprintf("cc: lex error\n");
    else if (rc == -4)
        kprintf("cc: compile error\n");
    else if (rc == -5)
        kprintf("cc: failed to write output\n");
    else
        kprintf("cc: failed (%d)\n", (uint64_t)(-rc));
}

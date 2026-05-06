/* cmd_cc.c — cc command: compile a C source file to an ELF binary */

#include "cc.h"
#include "libc.h"
#include "heap.h"
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

    /* Allocate compiler state on heap (it's ~250KB) */
    CompilerState *cc = (CompilerState *)kmalloc(sizeof(CompilerState));
    if (!cc) {
        kprintf("cc: out of memory\n");
        return;
    }
    memset(cc, 0, sizeof(CompilerState));

    /* Read source file */
    uint32_t read_sz = 0;
    int r = vfs_read(inpath, cc->src, CC_SRC_MAX - 1, &read_sz);
    if (r < 0 || read_sz == 0) {
        kprintf("cc: cannot read %s\n", inpath);
        kfree(cc);
        return;
    }
    cc->src[read_sz] = '\0';
    cc->src_len = read_sz;

    kprintf("cc: compiling %s (%u bytes)...\n", inpath, read_sz);

    /* Lex */
    cc_lex(cc);
    if (cc->error) {
        kprintf("cc: lex error: %s\n", cc->errmsg);
        kfree(cc);
        return;
    }
    kprintf("cc: %d tokens\n", cc->ntokens);

    /* Parse + codegen */
    cc_parse(cc);
    if (cc->error) {
        kprintf("cc: compile error: %s\n", cc->errmsg);
        kfree(cc);
        return;
    }
    kprintf("cc: code=%d bytes data=%d bytes\n", cc->code_len, cc->data_len);

    /* Write ELF */
    if (cc_write_elf(cc, outpath) < 0) {
        kprintf("cc: failed to write output\n");
        kfree(cc);
        return;
    }

    kprintf("cc: OK -> %s\n", outpath);
    kfree(cc);
}

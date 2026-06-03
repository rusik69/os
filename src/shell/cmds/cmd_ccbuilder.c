/* cmd_ccbuilder.c — manifest-driven C build orchestration command */

#include "shell_cmds.h"
#include "libc.h"
#include "string.h"
#include "printf.h"

#define CCB_MANIFEST_MAX 16384
#define CCB_PATH_MAX 256

static void ccb_default_out(const char *inpath, char outpath[CCB_PATH_MAX]) {
    strncpy(outpath, inpath, CCB_PATH_MAX - 1);
    outpath[CCB_PATH_MAX - 1] = '\0';
    int len = strlen(outpath);
    if (len > 2 && outpath[len - 2] == '.' && outpath[len - 1] == 'c') {
        outpath[len - 2] = '\0';
    }
}

static char *next_token(char **cursor) {
    char *p = *cursor;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) {
        *cursor = p;
        return 0;
    }
    char *tok = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    if (*p) {
        *p = '\0';
        p++;
    }
    *cursor = p;
    return tok;
}

static int ccb_compile_one(const char *src, const char *maybe_out) {
    char out[CCB_PATH_MAX];
    if (maybe_out && *maybe_out) {
        strncpy(out, maybe_out, CCB_PATH_MAX - 1);
        out[CCB_PATH_MAX - 1] = '\0';
    } else {
        ccb_default_out(src, out);
    }

    int rc = cc_compile(src, out);
    if (rc == 0)
        kprintf("ccbuilder: cc OK %s -> %s\n", src, out);
    else if (rc == -2)
        kprintf("ccbuilder: cc cannot read %s\n", src);
    else if (rc == -3)
        kprintf("ccbuilder: cc lex error in %s\n", src);
    else if (rc == -4)
        kprintf("ccbuilder: cc compile error in %s\n", src);
    else if (rc == -5)
        kprintf("ccbuilder: cc failed to write %s\n", out);
    else
        kprintf("ccbuilder: cc failed %s (%d)\n", src, (int)(-rc));
    return rc;
}

void cmd_ccbuilder(const char *args) {
    int keep_going = 0;
    const char *manifest = 0;

    if (!args || !*args) {
        kprintf("Usage: ccbuilder [-k|--keep-going] <manifest.txt>\n");
        kprintf("Manifest lines:\n");
        kprintf("  cc <source.c> [output]\n");
        kprintf("  exec <path>\n");
        kprintf("  run <script>\n");
        kprintf("  echo <text>\n");
        return;
    }

    if (strncmp(args, "-k ", 3) == 0) {
        keep_going = 1;
        manifest = args + 3;
    } else if (strncmp(args, "--keep-going ", 13) == 0) {
        keep_going = 1;
        manifest = args + 13;
    } else {
        manifest = args;
    }

    while (*manifest == ' ') manifest++;
    if (!*manifest) {
        kprintf("Usage: ccbuilder [-k|--keep-going] <manifest.txt>\n");
        return;
    }

    static char manifest_buf[CCB_MANIFEST_MAX];
    uint32_t sz = 0;
    if (fs_read_file(manifest, manifest_buf, CCB_MANIFEST_MAX - 1, &sz) < 0 || sz == 0) {
        kprintf("ccbuilder: cannot read manifest %s\n", manifest);
        return;
    }
    manifest_buf[sz] = '\0';

    int step_total = 0;
    int step_ok = 0;
    int step_fail = 0;
    int cc_ok = 0;
    int cc_fail = 0;
    int exec_ok = 0;
    int exec_fail = 0;
    int run_ok = 0;
    int run_fail = 0;

    int i = 0;
    int line_no = 0;
    while (manifest_buf[i]) {
        int line_start = i;
        while (manifest_buf[i] && manifest_buf[i] != '\n' && manifest_buf[i] != '\r') i++;
        int line_end = i;
        while (manifest_buf[i] == '\n' || manifest_buf[i] == '\r') i++;
        line_no++;

        if (line_end <= line_start) continue;

        char line[512];
        int line_len = line_end - line_start;
        if (line_len > (int)sizeof(line) - 1) line_len = (int)sizeof(line) - 1;
        memcpy(line, manifest_buf + line_start, line_len);
        line[line_len] = '\0';

        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#') continue;

        char *tail = p + strlen(p);
        while (tail > p && (tail[-1] == ' ' || tail[-1] == '\t')) tail--;
        *tail = '\0';
        if (!*p || *p == '#') continue;

        char *cursor = p;
        char *cmd = next_token(&cursor);
        if (!cmd) continue;

        step_total++;
        int rc = 0;

        if (strcmp(cmd, "cc") == 0) {
            char *src = next_token(&cursor);
            char *out = next_token(&cursor);
            if (!src || !*src) {
                kprintf("ccbuilder:%d: cc requires <source.c> [output]\n", line_no);
                rc = -1;
            } else {
                rc = ccb_compile_one(src, out);
            }
            if (rc == 0) cc_ok++; else cc_fail++;
            kprintf("ccbuilder: cc(ok=%d) %s\n", rc == 0 ? 1 : 0, src && *src ? src : "?");
        } else if (strcmp(cmd, "exec") == 0) {
            char *path = next_token(&cursor);
            if (!path || !*path) {
                kprintf("ccbuilder:%d: exec requires <path>\n", line_no);
                rc = -1;
            } else {
                rc = elf_exec(path);
                if (rc == 0)
                    kprintf("ccbuilder: exec OK %s\n", path);
                else
                    kprintf("ccbuilder: exec failed %s (%d)\n", path, (int)(-rc));
            }
            if (rc == 0) exec_ok++; else exec_fail++;
            kprintf("ccbuilder: exec(ok=%d) %s\n", rc == 0 ? 1 : 0, path ? path : "?");
        } else if (strcmp(cmd, "run") == 0) {
            char *path = next_token(&cursor);
            if (!path || !*path) {
                kprintf("ccbuilder:%d: run requires <script>\n", line_no);
                rc = -1;
            } else {
                rc = script_exec(path);
                if (rc == 0)
                    kprintf("ccbuilder: run OK %s\n", path);
                else
                    kprintf("ccbuilder: run failed %s (%d)\n", path, (int)(-rc));
            }
            if (rc == 0) run_ok++; else run_fail++;
            kprintf("ccbuilder: run(ok=%d) %s\n", rc == 0 ? 1 : 0, path ? path : "?");
        } else if (strcmp(cmd, "echo") == 0) {
            while (*cursor == ' ' || *cursor == '\t') cursor++;
            kprintf("ccbuilder: echo(ok=1) %s\n", cursor);
            rc = 0;
        } else {
            kprintf("ccbuilder:%d: unknown step '%s'\n", line_no, cmd);
            rc = -1;
        }

        if (rc == 0) {
            step_ok++;
        } else {
            step_fail++;
            if (!keep_going) {
                kprintf("ccbuilder: stopping on first error (use -k to continue)\n");
                break;
            }
        }
    }

    kprintf("ccbuilder: steps=%d ok=%d fail=%d\n", step_total, step_ok, step_fail);
    kprintf("ccbuilder: cc(ok=%d,fail=%d) exec(ok=%d,fail=%d) run(ok=%d,fail=%d)\n",
            cc_ok, cc_fail,
            exec_ok, exec_fail,
            run_ok, run_fail);
}

/* cmd_patch.c — apply unified diff format patches */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

#define PATCH_MAX_LINES 512
#define PATCH_MAX_LINE_LEN 256

void cmd_patch(const char *args) {
    if (!args || !*args) {
        kprintf("Usage: patch [options] <patchfile> [file]\n");
        return;
    }

    char argbuf[128];
    strncpy(argbuf, args, 127); argbuf[127] = '\0';

    char *patch_file = (char *)0;
    char *target_file = (char *)0;
    int strip_prefix = 0;
    int dry_run = 0;
    int reverse = 0;

    char *token = strtok(argbuf, " ");
    while (token) {
        if (strcmp(token, "-p0") == 0) strip_prefix = 0;
        else if (strcmp(token, "-p1") == 0) strip_prefix = 1;
        else if (strcmp(token, "--dry-run") == 0) dry_run = 1;
        else if (strcmp(token, "-R") == 0 || strcmp(token, "--reverse") == 0) reverse = 1;
        else if (token[0] == '-') { /* ignore other flags */ }
        else if (!patch_file) patch_file = token;
        else if (!target_file) target_file = token;
        token = strtok((char *)0, " ");
    }

    if (!patch_file) { kprintf("patch: no patch file specified\n"); return; }

    char pf_path[64];
    if (patch_file[0] != '/') { pf_path[0] = '/'; strncpy(pf_path + 1, patch_file, 62); pf_path[63] = '\0'; }
    else strncpy(pf_path, patch_file, 63);
    pf_path[63] = '\0';

    static unsigned char pf_buf[8192];
    uint32_t pf_size = 0;
    if (libc_vfs_read(pf_path, pf_buf, sizeof(pf_buf) - 1, &pf_size) != 0) {
        kprintf("patch: %s: not found\n", patch_file);
        return;
    }
    pf_buf[pf_size] = '\0';

    /* Parse unified diff patch */
    char *lines[PATCH_MAX_LINES];
    int num_lines = 0;
    char *line = (char *)pf_buf;
    while (*line && num_lines < PATCH_MAX_LINES) {
        lines[num_lines++] = line;
        while (*line && *line != '\n') line++;
        if (*line == '\n') *line++ = '\0';
    }

    /* Process hunks */
    int i = 0;
    int applied_changes = 0;

    /* Find file names from ---/+++ headers */
    char *file_a = (char *)0;
    char *file_b = (char *)0;

    while (i < num_lines) {
        if (strncmp(lines[i], "--- ", 4) == 0) {
            file_a = lines[i] + 4;
            /* Strip timestamp after tab */
            char *tab = strchr(file_a, '\t');
            if (tab) *tab = '\0';
            /* Strip prefix */
            for (int s = 0; s < strip_prefix; s++) {
                while (*file_a == '/') file_a++;
                while (*file_a && *file_a != '/') file_a++;
            }
            while (*file_a == '/') file_a++;
        }
        if (strncmp(lines[i], "+++ ", 4) == 0) {
            file_b = lines[i] + 4;
            char *tab = strchr(file_b, '\t');
            if (tab) *tab = '\0';
            for (int s = 0; s < strip_prefix; s++) {
                while (*file_b == '/') file_b++;
                while (*file_b && *file_b != '/') file_b++;
            }
            while (*file_b == '/') file_b++;
        }

        /* Parse hunk header: @@ -a,b +c,d @@ */
        if (strncmp(lines[i], "@@ -", 4) == 0) {
            const char *h = lines[i] + 4;
            int old_start = 0, new_start = 0;
            int old_count = 0, new_count = 0;

            while (*h >= '0' && *h <= '9') old_start = old_start * 10 + *h++ - '0';
            if (*h == ',') {
                h++;
                while (*h >= '0' && *h <= '9') old_count = old_count * 10 + *h++ - '0';
            } else old_count = 1;

            if (*h == ' ') h++;
            if (*h == '+') h++;
            while (*h >= '0' && *h <= '9') new_start = new_start * 10 + *h++ - '0';
            if (*h == ',') {
                h++;
                while (*h >= '0' && *h <= '9') new_count = new_count * 10 + *h++ - '0';
            } else new_count = 1;

            /* Determine target file */
            const char *tf = target_file ? target_file : (file_b ? file_b : (file_a ? file_a : "patch_output"));

            char tf_path[64];
            if (tf[0] != '/') { tf_path[0] = '/'; strncpy(tf_path + 1, tf, 62); tf_path[63] = '\0'; }
            else strncpy(tf_path, tf, 63);
            tf_path[63] = '\0';

            /* Read target file */
            static unsigned char tf_buf[8192];
            uint32_t tf_size = 0;
            uint32_t tf_alloc = sizeof(tf_buf) - 1;
            tf_buf[0] = '\0';

            int file_exists = (libc_vfs_read(tf_path, tf_buf, tf_alloc, &tf_size) == 0);
            if (!file_exists && old_start > 1) {
                kprintf("patch: %s: not found\n", tf);
                i++;
                continue;
            }
            if (file_exists) tf_buf[tf_size] = '\0';

            /* Build patched content */
            static unsigned char patched[16384];
            uint32_t ppos = 0;

            /* Parse the target file into lines */
            char *tf_lines[PATCH_MAX_LINES];
            int tf_num = 0;
            if (file_exists) {
                char *tl = (char *)tf_buf;
                while (*tl && tf_num < PATCH_MAX_LINES) {
                    tf_lines[tf_num++] = tl;
                    while (*tl && *tl != '\n') tl++;
                    if (*tl == '\n') *tl++ = '\0';
                }
            }

            /* Copy lines before the hunk */
            int old_line = 0;
            for (int j = 0; j < old_start - 1 && j < tf_num; j++) {
                if (ppos + strlen(tf_lines[j]) + 1 < sizeof(patched)) {
                    int len = strlen(tf_lines[j]);
                    memcpy(patched + ppos, tf_lines[j], len);
                    ppos += len;
                    patched[ppos++] = '\n';
                }
                old_line++;
            }

            /* Apply hunk */
            i++; /* Move past @@ line */
            int hunk_applied = 0;

            while (i < num_lines && lines[i][0] != '@') {
                if (lines[i][0] == '-') {
                    /* Remove line — skip in old file */
                    if (!reverse) {
                        if (old_line < tf_num) old_line++;
                    } else {
                        /* Reverse: add line */
                        if (ppos + strlen(lines[i] + 1) + 1 < sizeof(patched)) {
                            int len = strlen(lines[i] + 1);
                            memcpy(patched + ppos, lines[i] + 1, len);
                            ppos += len;
                            patched[ppos++] = '\n';
                        }
                    }
                    hunk_applied = 1;
                } else if (lines[i][0] == '+') {
                    /* Add line */
                    if (!reverse) {
                        if (ppos + strlen(lines[i] + 1) + 1 < sizeof(patched)) {
                            int len = strlen(lines[i] + 1);
                            memcpy(patched + ppos, lines[i] + 1, len);
                            ppos += len;
                            patched[ppos++] = '\n';
                        }
                    } else {
                        /* Reverse: skip this added line */
                    }
                    hunk_applied = 1;
                } else if (lines[i][0] == ' ') {
                    /* Context line — keep from old file */
                    if (old_line < tf_num) {
                        if (ppos + strlen(tf_lines[old_line]) + 1 < sizeof(patched)) {
                            int len = strlen(tf_lines[old_line]);
                            memcpy(patched + ppos, tf_lines[old_line], len);
                            ppos += len;
                            patched[ppos++] = '\n';
                        }
                        old_line++;
                    }
                }
                i++;
            }

            /* Copy remaining lines */
            while (old_line < tf_num) {
                if (ppos + strlen(tf_lines[old_line]) + 1 < sizeof(patched)) {
                    int len = strlen(tf_lines[old_line]);
                    memcpy(patched + ppos, tf_lines[old_line], len);
                    ppos += len;
                    patched[ppos++] = '\n';
                }
                old_line++;
            }

            if (ppos > 0 && patched[ppos-1] == '\n') ppos--;

            if (!dry_run && hunk_applied) {
                libc_vfs_write(tf_path, patched, ppos);
                applied_changes++;
            }
            if (hunk_applied) {
                kprintf("Hunk #%d applied to %s\n", applied_changes, tf);
            } else {
                kprintf("Hunk #%d failed for %s\n", applied_changes + 1, tf);
            }
            continue;
        }
        i++;
    }

    if (applied_changes == 0)
        kprintf("patch: no changes applied\n");
    else
        kprintf("patch: %d hunk(s) applied\n", (uint64_t)applied_changes);
}

/* cmd_script.c — record terminal session to file */
#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "libc.h"

void cmd_script(const char *args) {
    if (!args || !*args) {
        kprintf("Usage: script [file]\n");
        return;
    }

    char output_file[64];
    if (args[0] != '/') { output_file[0] = '/'; strncpy(output_file + 1, args, 62); }
    else strncpy(output_file, args, 63);
    output_file[63] = '\0';

    /* Remove trailing spaces */
    int len = strlen(output_file);
    while (len > 0 && output_file[len-1] == ' ') output_file[--len] = '\0';

    /* If no filename provided, use "typescript" */
    if (len == 0 || (len == 1 && output_file[0] == '/')) {
        strncpy(output_file, "/typescript", 63);
        output_file[63] = '\0';
    }

    kprintf("Script started, output file: %s\n", output_file);

    /* Record session: read characters from keyboard and write to file */
    char input_buf[256];
    unsigned char record_buf[4096];
    uint32_t rpos = 0;

    /* Write script header with timestamp */
    const char *header = "Script started on ";
    memcpy(record_buf + rpos, header, strlen(header));
    rpos += strlen(header);

    /* Get time if available */
    uint64_t secs = libc_time_seconds();
    /* Simple time string */
    char time_str[32];
    int ti = 0;
    uint64_t hours = (secs / 3600) % 24;
    uint64_t mins = (secs / 60) % 60;
    uint64_t sec = secs % 60;
    if (hours < 10) time_str[ti++] = '0';
    ti += snprintf(time_str + ti, 20, "%llu:", (unsigned long long)hours);
    if (mins < 10) time_str[ti++] = '0';
    ti += snprintf(time_str + ti, 20, "%llu:", (unsigned long long)mins);
    if (sec < 10) time_str[ti++] = '0';
    ti += snprintf(time_str + ti, 20, "%llu", (unsigned long long)sec);
    time_str[ti] = '\0';
    memcpy(record_buf + rpos, time_str, ti);
    rpos += ti;
    memcpy(record_buf + rpos, "\n", 1);
    rpos++;

    /* Read keyboard input and echo it back + record it */
    kprintf("Type 'exit' or Ctrl-D to end session.\n");

    while (1) {
        /* Read from pipe input if available */
        if (shell_has_stdin()) {
            int n = shell_stdin_read(input_buf, 255);
            if (n <= 0) break;
            input_buf[n] = '\0';

            /* Check for exit command */
            if (strcmp(input_buf, "exit\n") == 0 || strcmp(input_buf, "exit\r\n") == 0) {
                /* Write exit to output */
                if (rpos + 5 < sizeof(record_buf)) {
                    memcpy(record_buf + rpos, "exit\n", 5);
                    rpos += 5;
                }
                break;
            }

            /* Record input */
            if (rpos + (uint32_t)n < sizeof(record_buf)) {
                memcpy(record_buf + rpos, input_buf, (uint32_t)n);
                rpos += (uint32_t)n;
            }

            /* Echo input */
            for (int j = 0; j < n; j++)
                kprintf("%c", (unsigned long)(uint8_t)input_buf[j]);
        } else {
            /* No more input */
            break;
        }
    }

    /* Write script trailer */
    const char *trailer = "\nScript done on ";
    if (rpos + strlen(trailer) + ti + 1 < sizeof(record_buf)) {
        memcpy(record_buf + rpos, trailer, strlen(trailer));
        rpos += strlen(trailer);
        memcpy(record_buf + rpos, time_str, ti);
        rpos += ti;
        memcpy(record_buf + rpos, "\n", 1);
        rpos++;
    }

    /* Write recorded data to file */
    libc_vfs_write(output_file, record_buf, rpos);
    kprintf("Script done, output file: %s (%u bytes)\n", output_file, (unsigned long)rpos);
}

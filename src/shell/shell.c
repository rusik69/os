/* shell.c — Shell core: input loop, dispatch, history */

#include "shell.h"
#include "shell_cmds.h"
#include "vga.h"
#include "keyboard.h"
#include "printf.h"
#include "string.h"
#include "serial.h"
#include "editor.h"

static void putchar_both(char c) {
    vga_putchar(c);
    if (c == '\b') {
        serial_putchar('\b');
        serial_putchar(' ');
        serial_putchar('\b');
    } else {
        serial_putchar(c);
    }
}

#define CMD_BUF_SIZE 256
#define HISTORY_SIZE 16

static char cmd_buf[CMD_BUF_SIZE];
static int cmd_len;

static char history[HISTORY_SIZE][CMD_BUF_SIZE];
static int history_count = 0;
static int history_pos = 0;

static void history_add(const char *cmd) {
    if (cmd[0] == '\0') return;
    if (history_count > 0 && strcmp(history[(history_count - 1) % HISTORY_SIZE], cmd) == 0) return;
    strcpy(history[history_count % HISTORY_SIZE], cmd);
    history_count++;
}

void shell_history_add(const char *cmd_line) {
    history_add(cmd_line);
}

void shell_history_show_entries(void) {
    int start = history_count > HISTORY_SIZE ? history_count - HISTORY_SIZE : 0;
    for (int i = start; i < history_count; i++)
        kprintf("  %u: %s\n", (uint64_t)(i - start + 1), history[i % HISTORY_SIZE]);
}

static void shell_prompt(void) {
    vga_set_color(VGA_LIGHT_GREEN, VGA_BLACK);
    kprintf("os> ");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
}

static void erase_line(int len) {
    for (int i = 0; i < len; i++) putchar_both('\b');
    for (int i = 0; i < len; i++) putchar_both(' ');
    for (int i = 0; i < len; i++) putchar_both('\b');
}

static void process_cmd(void) {
    char *cmd = cmd_buf;
    while (*cmd == ' ') cmd++;
    if (*cmd == '\0') return;

    char *args = cmd;
    while (*args && *args != ' ') args++;
    if (*args) { *args = '\0'; args++; while (*args == ' ') args++; }
    else args = NULL;

    shell_exec_cmd(cmd, args);
}

void shell_exec_cmd(const char *cmd, const char *args) {
    if (strcmp(cmd, "help") == 0) cmd_help();
    else if (strcmp(cmd, "echo") == 0) cmd_echo(args);
    else if (strcmp(cmd, "clear") == 0) vga_clear();
    else if (strcmp(cmd, "meminfo") == 0) cmd_meminfo();
    else if (strcmp(cmd, "ps") == 0) cmd_ps();
    else if (strcmp(cmd, "uptime") == 0) cmd_uptime();
    else if (strcmp(cmd, "reboot") == 0) cmd_reboot();
    else if (strcmp(cmd, "shutdown") == 0) cmd_shutdown();
    else if (strcmp(cmd, "kill") == 0) cmd_kill(args);
    else if (strcmp(cmd, "color") == 0) cmd_color(args);
    else if (strcmp(cmd, "hexdump") == 0) cmd_hexdump(args);
    else if (strcmp(cmd, "date") == 0) cmd_date();
    else if (strcmp(cmd, "cpuinfo") == 0) cmd_cpuinfo();
    else if (strcmp(cmd, "history") == 0) cmd_history_show();
    else if (strcmp(cmd, "ls") == 0) cmd_ls(args);
    else if (strcmp(cmd, "cat") == 0) cmd_cat(args);
    else if (strcmp(cmd, "write") == 0) cmd_write(args);
    else if (strcmp(cmd, "touch") == 0) cmd_touch(args);
    else if (strcmp(cmd, "rm") == 0) cmd_rm(args);
    else if (strcmp(cmd, "mkdir") == 0) cmd_mkdir(args);
    else if (strcmp(cmd, "stat") == 0) cmd_stat_file(args);
    else if (strcmp(cmd, "format") == 0) cmd_format_disk();
    else if (strcmp(cmd, "edit") == 0) editor_open(args);
    else if (strcmp(cmd, "exec") == 0) cmd_exec(args);
    else if (strcmp(cmd, "run") == 0) cmd_run(args);
    else if (strcmp(cmd, "beep") == 0) cmd_beep(args);
    else if (strcmp(cmd, "play") == 0) cmd_play(args);
    else if (strcmp(cmd, "mouse") == 0) cmd_mouse_status();
    else if (strcmp(cmd, "udpsend") == 0) cmd_udpsend(args);
    else if (strcmp(cmd, "ifconfig") == 0) cmd_ifconfig();
    else if (strcmp(cmd, "ping") == 0) cmd_ping(args);
    else if (strcmp(cmd, "dns") == 0) cmd_dns(args);
    else if (strcmp(cmd, "curl") == 0) cmd_curl(args);
    else if (strcmp(cmd, "wc") == 0) cmd_wc(args);
    else if (strcmp(cmd, "head") == 0) cmd_head(args);
    else if (strcmp(cmd, "tail") == 0) cmd_tail(args);
    else if (strcmp(cmd, "cp") == 0) cmd_cp(args);
    else if (strcmp(cmd, "mv") == 0) cmd_mv(args);
    else if (strcmp(cmd, "grep") == 0) cmd_grep(args);
    else if (strcmp(cmd, "df") == 0) cmd_df();
    else if (strcmp(cmd, "free") == 0) cmd_free();
    else if (strcmp(cmd, "whoami") == 0) cmd_whoami();
    else if (strcmp(cmd, "hostname") == 0) cmd_hostname();
    else if (strcmp(cmd, "env") == 0) cmd_env();
    else if (strcmp(cmd, "xxd") == 0) cmd_xxd(args);
    else if (strcmp(cmd, "sleep") == 0) cmd_sleep(args);
    else if (strcmp(cmd, "seq") == 0) cmd_seq(args);
    else if (strcmp(cmd, "arp") == 0) cmd_arp();
    else if (strcmp(cmd, "route") == 0) cmd_route();
    else if (strcmp(cmd, "uname") == 0) cmd_uname();
    else if (strcmp(cmd, "lspci") == 0) cmd_lspci();
    else if (strcmp(cmd, "dmesg") == 0) cmd_dmesg();
    else kprintf("Unknown command: %s\n", cmd);
}

void shell_run(void) {
    kprintf("\nWelcome to the OS shell. Type 'help' for commands.\n\n");

    for (;;) {
        shell_prompt();
        cmd_len = 0;
        memset(cmd_buf, 0, CMD_BUF_SIZE);
        history_pos = history_count;

        while (1) {
            char c = keyboard_getchar();

            if (c == '\n') {
                putchar_both('\n');
                cmd_buf[cmd_len] = '\0';
                history_add(cmd_buf);
                process_cmd();
                break;
            } else if (c == KEY_UP) {
                if (history_pos > 0 && history_pos > history_count - HISTORY_SIZE) {
                    history_pos--;
                    erase_line(cmd_len);
                    strcpy(cmd_buf, history[history_pos % HISTORY_SIZE]);
                    cmd_len = strlen(cmd_buf);
                    kprintf("%s", cmd_buf);
                }
            } else if (c == KEY_DOWN) {
                erase_line(cmd_len);
                if (history_pos < history_count - 1) {
                    history_pos++;
                    strcpy(cmd_buf, history[history_pos % HISTORY_SIZE]);
                    cmd_len = strlen(cmd_buf);
                    kprintf("%s", cmd_buf);
                } else {
                    history_pos = history_count;
                    cmd_buf[0] = '\0';
                    cmd_len = 0;
                }
            } else if (c == '\b') {
                if (cmd_len > 0) {
                    cmd_len--;
                    putchar_both('\b');
                }
            } else if (cmd_len < CMD_BUF_SIZE - 1) {
                cmd_buf[cmd_len++] = c;
                putchar_both(c);
            }
        }
    }
}

void shell_init(void) {
}

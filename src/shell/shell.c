/* shell.c — Shell core: input loop, dispatch, history */

#include "shell.h"
#include "shell_cmds.h"
#include "vga.h"
#include "keyboard.h"
#include "printf.h"
#include "string.h"
#include "serial.h"
#include "editor.h"
#include "vfs.h"
#include "fs.h"
#include "pipe.h"
#include "process.h"
#include "scheduler.h"

#define MAX_VAR_NAME 32

/* Expand $VAR references in src into dst (dst_max includes NUL) */
static void var_expand(const char *src, char *dst, int dst_max) {
    int di = 0;
    while (*src && di < dst_max - 1) {
        if (*src == '$') {
            src++;
            char name[MAX_VAR_NAME];
            int ni = 0;
            while (*src && ni < MAX_VAR_NAME - 1 &&
                   ((*src >= 'A' && *src <= 'Z') ||
                    (*src >= 'a' && *src <= 'z') ||
                    (*src >= '0' && *src <= '9') ||
                    *src == '_')) {
                name[ni++] = *src++;
            }
            name[ni] = '\0';
            const char *val = shell_var_get(name);
            while (*val && di < dst_max - 1)
                dst[di++] = *val++;
        } else {
            dst[di++] = *src++;
        }
    }
    dst[di] = '\0';
}

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

static char cmd_buf[CMD_BUF_SIZE];
static int cmd_len;

static char history[HISTORY_SIZE][CMD_BUF_SIZE];
static int history_count = 0;
static int history_pos = 0;

#define HISTORY_FILE  "/history"
#define HISTORY_FILE_MAX  (HISTORY_SIZE * CMD_BUF_SIZE)

static char history_filebuf[HISTORY_FILE_MAX];

/* History functions extracted to sub-module */
#include "shell_history.inc"

/* Command table + tab completion extracted to sub-module */
#include "shell_completion.inc"

/* --- Background process support --- */
struct bg_cmd_info {
    char cmd[64];
    char args[CMD_BUF_SIZE];
    int  has_args;
};
static struct bg_cmd_info bg_slots[8];
static int bg_slot_next = 0;

static void bg_cmd_entry(void) {
    /* Find our slot by matching process name to bg_slots[].cmd */
    struct process *me = process_get_current();
    int slot = -1;
    for (int i = 0; i < 8; i++) {
        if (me->name == bg_slots[i].cmd) { slot = i; break; }
    }
    if (slot < 0) { process_exit(); return; }
    const char *a = bg_slots[slot].has_args ? bg_slots[slot].args : NULL;
    shell_exec_cmd(bg_slots[slot].cmd, a);
    process_exit();
}

static void process_cmd(void) {
    char *cmd = cmd_buf;
    while (*cmd == ' ') cmd++;
    if (*cmd == '\0') return;

    /* --- Variable expansion: replace $VAR with its value --- */
    static char expanded[CMD_BUF_SIZE];
    var_expand(cmd_buf, expanded, CMD_BUF_SIZE);
    strncpy(cmd_buf, expanded, CMD_BUF_SIZE - 1);
    cmd_buf[CMD_BUF_SIZE - 1] = '\0';
    cmd = cmd_buf;
    while (*cmd == ' ') cmd++;
    if (*cmd == '\0') return;

    /* --- Check for variable assignment: NAME=value (no spaces around =) --- */
    {
        const char *p = cmd;
        int valid_name = 1;
        while (*p && *p != '=' && *p != ' ') {
            char c = *p;
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '_'))
                { valid_name = 0; break; }
            p++;
        }
        if (valid_name && *p == '=' && p > cmd) {
            char name[MAX_VAR_NAME];
            int nl = (int)(p - cmd);
            if (nl > MAX_VAR_NAME - 1) nl = MAX_VAR_NAME - 1;
            memcpy(name, cmd, nl);
            name[nl] = '\0';
            shell_var_set(name, p + 1);
            return;
        }
    }

    /* --- Check for background operator: cmd & --- */
    int bg = 0;
    {
        int len = strlen(cmd);
        while (len > 0 && cmd[len-1] == ' ') len--;
        if (len > 0 && cmd[len-1] == '&') {
            bg = 1;
            cmd[len-1] = '\0';
            len--;
            while (len > 0 && cmd[len-1] == ' ') { cmd[len-1] = '\0'; len--; }
        }
    }

    if (bg) {
        /* Parse command and args, then launch as background process */
        char *bcmd = cmd;
        while (*bcmd == ' ') bcmd++;
        char *bargs = bcmd;
        while (*bargs && *bargs != ' ') bargs++;
        if (*bargs) { *bargs = '\0'; bargs++; while (*bargs == ' ') bargs++; }
        else bargs = NULL;

        int slot = bg_slot_next;
        bg_slot_next = (bg_slot_next + 1) % 8;
        strncpy(bg_slots[slot].cmd, bcmd, 63);
        bg_slots[slot].cmd[63] = '\0';
        if (bargs && *bargs) {
            strncpy(bg_slots[slot].args, bargs, CMD_BUF_SIZE - 1);
            bg_slots[slot].args[CMD_BUF_SIZE - 1] = '\0';
            bg_slots[slot].has_args = 1;
        } else {
            bg_slots[slot].args[0] = '\0';
            bg_slots[slot].has_args = 0;
        }

        struct process *p = process_create(bg_cmd_entry, bg_slots[slot].cmd);
        if (p) {
            p->is_background = 1;
            kprintf("[%u] %s\n", (uint64_t)p->pid, bg_slots[slot].cmd);
        } else {
            kprintf("Failed to create background process\n");
        }
        return;
    }

    /* --- Check for pipe: cmd1 | cmd2 --- */
    char *pipe_pos = 0;
    for (char *p = cmd; *p; p++) {
        if (*p == '|') { pipe_pos = p; break; }
    }
    if (pipe_pos) {
        *pipe_pos = '\0';
        char *left = cmd;
        char *right = pipe_pos + 1;
        while (*right == ' ') right++;
        /* Trim trailing spaces from left */
        char *lt = pipe_pos - 1;
        while (lt > left && *lt == ' ') { *lt = '\0'; lt--; }

        /* Create a temporary pipe file */
        const char *pipe_file = "/.pipe_tmp";
        /* Execute left side, capturing output to pipe file */
        /* We use kprintf output redirect via a buffer */
        static char pipe_buf[4096];
        int pipe_len = 0;
        /* Save kprintf hook state and redirect output */
        void (*saved_hook)(char, void*) = 0;
        void *saved_ctx = 0;
        kprintf_get_hook(&saved_hook, &saved_ctx);

        /* Parse left command */
        char *lcmd = left; while (*lcmd == ' ') lcmd++;
        char *largs = lcmd;
        while (*largs && *largs != ' ') largs++;
        if (*largs) { *largs = '\0'; largs++; while (*largs == ' ') largs++; }
        else largs = 0;

        /* Execute left, capture to pipe_buf via kprintf hook */
        pipe_len = 0;
        void pipe_capture(char c, void *ctx) {
            (void)ctx;
            if (pipe_len < 4095) pipe_buf[pipe_len++] = c;
        }
        kprintf_set_hook(pipe_capture, 0);
        shell_exec_cmd(lcmd, largs);
        kprintf_set_hook(saved_hook, saved_ctx);
        pipe_buf[pipe_len] = '\0';

        /* Write captured output to temp file */
        vfs_write(pipe_file, pipe_buf, (uint32_t)pipe_len);

        /* Parse right command and inject pipe file as arg */
        char *rcmd = right;
        char *rargs = rcmd;
        while (*rargs && *rargs != ' ') rargs++;
        if (*rargs) { *rargs = '\0'; rargs++; while (*rargs == ' ') rargs++; }
        else rargs = 0;

        /* For pipe: append pipe_file as last arg if no file arg given */
        char combined_args[CMD_BUF_SIZE];
        if (rargs && rargs[0]) {
            int n = strlen(rargs);
            memcpy(combined_args, rargs, n);
            combined_args[n] = ' ';
            strcpy(combined_args + n + 1, pipe_file);
        } else {
            strcpy(combined_args, pipe_file);
        }
        shell_exec_cmd(rcmd, combined_args);
        vfs_unlink(pipe_file);
        return;
    }

    /* --- Check for output redirection: cmd > file or cmd >> file --- */
    char *redir_pos = 0;
    int redir_append = 0;
    for (char *p = cmd; *p; p++) {
        if (*p == '>' && *(p + 1) == '>') { redir_pos = p; redir_append = 1; break; }
        if (*p == '>') { redir_pos = p; break; }
    }
    if (redir_pos) {
        *redir_pos = '\0';
        char *file = redir_pos + 1;
        if (redir_append) { file++; }
        while (*file == ' ') file++;
        /* trim trailing spaces from file */
        int flen = strlen(file);
        while (flen > 0 && file[flen-1] == ' ') file[--flen] = '\0';
        /* prefix with / if needed */
        char filepath[64];
        if (file[0] != '/') {
            filepath[0] = '/';
            strncpy(filepath + 1, file, 62);
        } else {
            strncpy(filepath, file, 63);
        }
        filepath[63] = '\0';

        /* Trim left side and parse */
        char *lcmd = cmd; while (*lcmd == ' ') lcmd++;
        char *lt = redir_pos - 1;
        while (lt > lcmd && *lt == ' ') { *lt = '\0'; lt--; }
        char *largs = lcmd;
        while (*largs && *largs != ' ') largs++;
        if (*largs) { *largs = '\0'; largs++; while (*largs == ' ') largs++; }
        else largs = 0;

        /* Capture output */
        static char redir_buf[4096];
        int redir_len = 0;
        void (*saved_hook)(char, void*) = 0;
        void *saved_ctx = 0;
        kprintf_get_hook(&saved_hook, &saved_ctx);
        void redir_capture(char c, void *ctx) {
            (void)ctx;
            if (redir_len < 4095) redir_buf[redir_len++] = c;
        }
        kprintf_set_hook(redir_capture, 0);
        shell_exec_cmd(lcmd, largs);
        kprintf_set_hook(saved_hook, saved_ctx);
        redir_buf[redir_len] = '\0';

        if (redir_append) {
            /* Read existing content, append */
            uint32_t existing = 0;
            char old[4096];
            if (vfs_read(filepath, old, 4095, &existing) == 0 && existing > 0) {
                int total = (int)existing + redir_len;
                if (total > 4095) total = 4095;
                memcpy(old + existing, redir_buf, total - (int)existing);
                old[total] = '\0';
                vfs_write(filepath, old, (uint32_t)total);
            } else {
                vfs_create(filepath, 1);
                vfs_write(filepath, redir_buf, (uint32_t)redir_len);
            }
        } else {
            vfs_create(filepath, 1);
            vfs_write(filepath, redir_buf, (uint32_t)redir_len);
        }
        return;
    }

    /* --- Normal command --- */
    char *args = cmd;
    while (*args && *args != ' ') args++;
    if (*args) { *args = '\0'; args++; while (*args == ' ') args++; }
    else args = NULL;

    shell_exec_cmd(cmd, args);
}

/* Process a full command line (with pipe/redirect/background support).
 * Used by telnet daemon to get the same features as the local shell. */
void shell_process_line(const char *line) {
    if (!line || !*line) return;
    strncpy(cmd_buf, line, CMD_BUF_SIZE - 1);
    cmd_buf[CMD_BUF_SIZE - 1] = '\0';
    cmd_len = strlen(cmd_buf);
    process_cmd();
}

void shell_exec_cmd(const char *cmd, const char *args) {
    /* Per-command --help */
    if (args && strcmp(args, "--help") == 0) {
        if (strcmp(cmd, "echo") == 0)
            kprintf("Usage: echo [text]\n  Print text to console\n");
        else if (strcmp(cmd, "clear") == 0)
            kprintf("Usage: clear\n  Clear the screen\n");
        else if (strcmp(cmd, "meminfo") == 0)
            kprintf("Usage: meminfo\n  Show physical memory statistics\n");
        else if (strcmp(cmd, "ps") == 0)
            kprintf("Usage: ps\n  List all processes and their states\n");
        else if (strcmp(cmd, "uptime") == 0)
            kprintf("Usage: uptime\n  Show time since boot\n");
        else if (strcmp(cmd, "reboot") == 0)
            kprintf("Usage: reboot\n  Reboot the system\n");
        else if (strcmp(cmd, "shutdown") == 0)
            kprintf("Usage: shutdown\n  Power off via ACPI\n");
        else if (strcmp(cmd, "kill") == 0)
            kprintf("Usage: kill <pid> [signal]\n  Send signal to process (default signal 9 = SIGKILL)\n");
        else if (strcmp(cmd, "color") == 0)
            kprintf("Usage: color <fg> [bg]\n  Set console text/background color (0-15)\n");
        else if (strcmp(cmd, "hexdump") == 0)
            kprintf("Usage: hexdump <addr> [len]\n  Dump memory at address in hex (default len=64, max 256)\n");
        else if (strcmp(cmd, "date") == 0)
            kprintf("Usage: date\n  Show current date and time from RTC\n");
        else if (strcmp(cmd, "cpuinfo") == 0)
            kprintf("Usage: cpuinfo\n  Show CPU vendor and brand string\n");
        else if (strcmp(cmd, "history") == 0)
            kprintf("Usage: history\n  Show recent command history\n");
        else if (strcmp(cmd, "ls") == 0)
            kprintf("Usage: ls [path]\n  List directory contents (default /)\n");
        else if (strcmp(cmd, "cat") == 0)
            kprintf("Usage: cat <file>\n  Print file contents\n");
        else if (strcmp(cmd, "write") == 0)
            kprintf("Usage: write <file> <text>\n  Write text to file\n");
        else if (strcmp(cmd, "touch") == 0)
            kprintf("Usage: touch <file>\n  Create empty file\n");
        else if (strcmp(cmd, "rm") == 0)
            kprintf("Usage: rm <path>\n  Remove file or directory\n");
        else if (strcmp(cmd, "mkdir") == 0)
            kprintf("Usage: mkdir <dir>\n  Create directory\n");
        else if (strcmp(cmd, "stat") == 0)
            kprintf("Usage: stat <path>\n  Show file or directory metadata (type, size)\n");
        else if (strcmp(cmd, "format") == 0)
            kprintf("Usage: format\n  Format the filesystem (WARNING: destroys all data)\n");
        else if (strcmp(cmd, "edit") == 0)
            kprintf("Usage: edit <file>\n  Open file in text editor (Ctrl-S save, Ctrl-Q quit)\n");
        else if (strcmp(cmd, "exec") == 0)
            kprintf("Usage: exec <path>\n  Load and run a static ELF64 binary\n");
        else if (strcmp(cmd, "run") == 0)
            kprintf("Usage: run <path>\n  Execute a shell script file\n");
        else if (strcmp(cmd, "ifconfig") == 0)
            kprintf("Usage: ifconfig\n  Show network interface configuration (MAC, IP, mask, gateway)\n");
        else if (strcmp(cmd, "ping") == 0)
            kprintf("Usage: ping [ip|hostname]\n  Send ICMP echo request (default: gateway)\n");
        else if (strcmp(cmd, "dns") == 0)
            kprintf("Usage: dns <hostname>\n  Resolve hostname to IP address\n");
        else if (strcmp(cmd, "curl") == 0)
            kprintf("Usage: curl [-F] <url>\n  HTTP GET request. -F follows redirects (301/302/303/307/308, max 5)\n  Example: curl -F http://example.com/\n");
        else if (strcmp(cmd, "udpsend") == 0)
            kprintf("Usage: udpsend <ip> <port> <data>\n  Send a UDP datagram\n");
        else if (strcmp(cmd, "beep") == 0)
            kprintf("Usage: beep [freq] [ms]\n  Play tone on PC speaker (default 1000 Hz, 200 ms)\n");
        else if (strcmp(cmd, "play") == 0)
            kprintf("Usage: play <note> [note ...]\n  Play musical notes (C4 D4 E4 F4 G4 A4 B4 C5)\n");
        else if (strcmp(cmd, "mouse") == 0)
            kprintf("Usage: mouse\n  Show current mouse position and button state\n");
        else if (strcmp(cmd, "wc") == 0)
            kprintf("Usage: wc <file>\n  Count lines, words, and bytes in file\n");
        else if (strcmp(cmd, "head") == 0)
            kprintf("Usage: head <file> [n]\n  Show first n lines of file (default 10)\n");
        else if (strcmp(cmd, "tail") == 0)
            kprintf("Usage: tail <file> [n]\n  Show last n lines of file (default 10)\n");
        else if (strcmp(cmd, "cp") == 0)
            kprintf("Usage: cp <src> <dst>\n  Copy file\n");
        else if (strcmp(cmd, "mv") == 0)
            kprintf("Usage: mv <src> <dst>\n  Move or rename file\n");
        else if (strcmp(cmd, "grep") == 0)
            kprintf("Usage: grep <pattern> <file>\n  Search for pattern in file\n");
        else if (strcmp(cmd, "df") == 0)
            kprintf("Usage: df\n  Show disk usage statistics\n");
        else if (strcmp(cmd, "free") == 0)
            kprintf("Usage: free\n  Show memory usage (total/used/free)\n");
        else if (strcmp(cmd, "whoami") == 0)
            kprintf("Usage: whoami\n  Show current process PID and name\n");
        else if (strcmp(cmd, "hostname") == 0)
            kprintf("Usage: hostname\n  Print system hostname\n");
        else if (strcmp(cmd, "env") == 0)
            kprintf("Usage: env\n  Print environment variables (PID, NAME, IP, UPTIME, ...)\n");
        else if (strcmp(cmd, "xxd") == 0)
            kprintf("Usage: xxd <file>\n  Hex dump file contents (first 256 bytes)\n");
        else if (strcmp(cmd, "sleep") == 0)
            kprintf("Usage: sleep <seconds>\n  Pause for n seconds (max 60)\n");
        else if (strcmp(cmd, "seq") == 0)
            kprintf("Usage: seq <end>  or  seq <start> <end>\n  Print integer sequence\n");
        else if (strcmp(cmd, "arp") == 0)
            kprintf("Usage: arp\n  Show ARP cache entries\n");
        else if (strcmp(cmd, "route") == 0)
            kprintf("Usage: route\n  Show routing table\n");
        else if (strcmp(cmd, "uname") == 0)
            kprintf("Usage: uname\n  Print system information\n");
        else if (strcmp(cmd, "lspci") == 0)
            kprintf("Usage: lspci\n  List PCI devices\n");
        else if (strcmp(cmd, "dmesg") == 0)
            kprintf("Usage: dmesg\n  Show kernel boot log\n");
        else if (strcmp(cmd, "cc") == 0)
            kprintf("Usage: cc <source.c> [output]\n  Compile C source to static ELF64 binary\n");
        else if (strcmp(cmd, "ccbuilder") == 0)
            kprintf("Usage: ccbuilder [-k|--keep-going] <manifest.txt>\n  Run manifest steps: cc/exec/run/echo\n");
        else if (strcmp(cmd, "sort") == 0)
            kprintf("Usage: sort <file>\n  Sort lines of a file alphabetically\n");
        else if (strcmp(cmd, "find") == 0)
            kprintf("Usage: find <pattern>\n  Search for files matching pattern\n");
        else if (strcmp(cmd, "calc") == 0)
            kprintf("Usage: calc <expression>\n  Evaluate arithmetic expression (+, -, *, /, %%, parens)\n");
        else if (strcmp(cmd, "uniq") == 0)
            kprintf("Usage: uniq <file>\n  Remove adjacent duplicate lines\n");
        else if (strcmp(cmd, "tr") == 0)
            kprintf("Usage: tr <from> <to> <file>\n  Translate characters in file\n");
        else if (strcmp(cmd, "tmux") == 0)
            kprintf("Usage: tmux\n  Terminal multiplexer (Ctrl-B prefix, see tmux --help)\n");
        else if (strcmp(cmd, "jobs") == 0)
            kprintf("Usage: jobs\n  List background processes\n");
        else if (strcmp(cmd, "fg") == 0)
            kprintf("Usage: fg <pid>\n  Bring background process to foreground\n");
        else if (strcmp(cmd, "wait") == 0)
            kprintf("Usage: wait <pid>\n  Wait for a process to finish\n");
        else if (strcmp(cmd, "help") == 0)
            kprintf("Usage: help\n  List all available commands\n");
        else if (strcmp(cmd, "exit") == 0)
            kprintf("Usage: exit\n  Disconnect telnet session\n");
        else if (strcmp(cmd, "printf") == 0)
            kprintf("Usage: printf <format> [args...]\n  Format and print. Supports \\n \\t %%s %%d\n");
        else if (strcmp(cmd, "time") == 0)
            kprintf("Usage: time <command> [args...]\n  Measure execution time of a command\n");
        else if (strcmp(cmd, "strings") == 0)
            kprintf("Usage: strings <file>\n  Print printable strings found in a file\n");
        else if (strcmp(cmd, "tac") == 0)
            kprintf("Usage: tac <file>\n  Print file lines in reverse order\n");
        else if (strcmp(cmd, "base64") == 0)
            kprintf("Usage: base64 <file>\n  Encode file contents as base64\n");
        else if (strcmp(cmd, "cmos") == 0)
            kprintf("Usage: cmos\n  Show CMOS/NVRAM hardware configuration\n");
        else if (strcmp(cmd, "hwinfo") == 0)
            kprintf("Usage: hwinfo\n  Show comprehensive hardware information\n");
        else if (strcmp(cmd, "fbinfo") == 0)
            kprintf("Usage: fbinfo\n  Show active display backend and framebuffer geometry\n");
        else if (strcmp(cmd, "gui") == 0)
            kprintf("Usage: gui\n  Launch GUI desktop environment (experimental)\n");
        else if (strcmp(cmd, "serial") == 0)
            kprintf("Usage: serial status | serial write <text>\n  COM1 serial port operations\n");
        else
            kprintf("Unknown command: %s\n", cmd);
        return;
    }

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
    else if (strcmp(cmd, "cc") == 0) cmd_cc(args);
    else if (strcmp(cmd, "ccbuilder") == 0) cmd_ccbuilder(args);
    else if (strcmp(cmd, "sort") == 0) cmd_sort(args);
    else if (strcmp(cmd, "find") == 0) cmd_find(args);
    else if (strcmp(cmd, "calc") == 0) cmd_calc(args);
    else if (strcmp(cmd, "uniq") == 0) cmd_uniq(args);
    else if (strcmp(cmd, "tr") == 0) cmd_tr(args);
    else if (strcmp(cmd, "tmux") == 0) cmd_tmux(args);
    else if (strcmp(cmd, "jobs") == 0) cmd_jobs();
    else if (strcmp(cmd, "fg") == 0) cmd_fg(args);
    else if (strcmp(cmd, "wait") == 0) cmd_wait(args);
    else if (strcmp(cmd, "tee") == 0) cmd_tee(args);
    else if (strcmp(cmd, "cut") == 0) cmd_cut(args);
    else if (strcmp(cmd, "paste") == 0) cmd_paste(args);
    else if (strcmp(cmd, "basename") == 0) cmd_basename(args);
    else if (strcmp(cmd, "dirname") == 0) cmd_dirname(args);
    else if (strcmp(cmd, "yes") == 0) cmd_yes(args);
    else if (strcmp(cmd, "rev") == 0) cmd_rev(args);
    else if (strcmp(cmd, "nl") == 0) cmd_nl(args);
    else if (strcmp(cmd, "du") == 0) cmd_du(args);
    else if (strcmp(cmd, "id") == 0) cmd_id(args);
    else if (strcmp(cmd, "diff") == 0) cmd_diff(args);
    else if (strcmp(cmd, "md5sum") == 0) cmd_md5sum(args);
    else if (strcmp(cmd, "od") == 0) cmd_od(args);
    else if (strcmp(cmd, "expr") == 0) cmd_expr(args);
    else if (strcmp(cmd, "test") == 0) cmd_test(args);
    else if (strcmp(cmd, "[") == 0) cmd_test(args);
    else if (strcmp(cmd, "xargs") == 0) cmd_xargs(args);
    else if (strcmp(cmd, "printf") == 0) cmd_printf(args);
    else if (strcmp(cmd, "time") == 0) cmd_time(args);
    else if (strcmp(cmd, "strings") == 0) cmd_strings(args);
    else if (strcmp(cmd, "tac") == 0) cmd_tac(args);
    else if (strcmp(cmd, "base64") == 0) cmd_base64(args);
    else if (strcmp(cmd, "cmos") == 0) cmd_cmos();
    else if (strcmp(cmd, "hwinfo") == 0) cmd_hwinfo();
    else if (strcmp(cmd, "fbinfo") == 0) cmd_fbinfo();
    else if (strcmp(cmd, "gui") == 0) cmd_gui();
    else if (strcmp(cmd, "serial") == 0) cmd_serial(args);
    else if (strcmp(cmd, "lsusb") == 0) cmd_lsusb();
    else if (strcmp(cmd, "lsblk") == 0) cmd_lsblk();
    else if (strcmp(cmd, "fat") == 0) cmd_fat(args);
    else if (strcmp(cmd, "chmod") == 0) cmd_chmod(args);
    else if (strcmp(cmd, "chown") == 0) cmd_chown(args);
    else if (strcmp(cmd, "login") == 0) cmd_login(args);
    else if (strcmp(cmd, "logout") == 0) cmd_logout();
    else if (strcmp(cmd, "useradd") == 0) cmd_useradd(args);
    else if (strcmp(cmd, "userdel") == 0) cmd_userdel(args);
    else if (strcmp(cmd, "passwd") == 0) cmd_passwd(args);
    else if (strcmp(cmd, "users") == 0) cmd_users();
    else if (strcmp(cmd, "capprof") == 0) cmd_capprof(args);
    else kprintf("Unknown command: %s\n", cmd);
}

void shell_run(void) {
    history_load();
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
            } else if (c == '\t') {
                cmd_buf[cmd_len] = '\0';
                shell_tab_complete(cmd_buf, &cmd_len);
            } else if (cmd_len < CMD_BUF_SIZE - 1) {
                cmd_buf[cmd_len++] = c;
                putchar_both(c);
            }
        }
    }
}

void shell_init(void) {
}

/* Read a line from keyboard into buf (up to max-1 chars) */
void shell_read_line(char *buf, int max) {
    int len = 0;
    while (1) {
        char c = keyboard_getchar();
        if (c == '\n') {
            putchar_both('\n');
            break;
        } else if (c == '\b') {
            if (len > 0) {
                len--;
                putchar_both('\b');
            }
        } else if (len < max - 1) {
            buf[len++] = c;
            putchar_both(c);
        }
    }
    buf[len] = '\0';
}

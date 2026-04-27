/* shell_cmds.c — Original shell command implementations */

#include "shell_cmds.h"
#include "vga.h"
#include "printf.h"
#include "string.h"
#include "timer.h"
#include "pmm.h"
#include "process.h"
#include "scheduler.h"
#include "io.h"
#include "fs.h"
#include "ata.h"
#include "editor.h"
#include "net.h"
#include "e1000.h"
#include "rtc.h"
#include "mouse.h"
#include "speaker.h"
#include "acpi.h"
#include "signal.h"
#include "pipe.h"
#include "script.h"
#include "elf.h"
#include "vfs.h"
#include "shell.h"

void cmd_help(void) {
    kprintf("Available commands:\n");
    kprintf("  help     - Show this help\n");
    kprintf("  echo     - Print arguments\n");
    kprintf("  clear    - Clear screen\n");
    kprintf("  meminfo  - Show memory info\n");
    kprintf("  ps       - List processes\n");
    kprintf("  uptime   - Show uptime\n");
    kprintf("  reboot   - Reboot system\n");
    kprintf("  shutdown - Shutdown system (ACPI)\n");
    kprintf("  kill     - Kill process (kill <pid> [signal])\n");
    kprintf("  color    - Set color (color <fg> [bg])\n");
    kprintf("  hexdump  - Dump memory (hexdump <addr> [len])\n");
    kprintf("  date     - Show current date/time from RTC\n");
    kprintf("  cpuinfo  - Show CPU info\n");
    kprintf("  history  - Show command history\n");
    kprintf("  ls       - List files [dir]\n");
    kprintf("  cat      - Show file contents\n");
    kprintf("  write    - Write to file (write <name> <text>)\n");
    kprintf("  touch    - Create empty file\n");
    kprintf("  rm       - Remove file or empty dir\n");
    kprintf("  mkdir    - Create directory\n");
    kprintf("  stat     - Show file info\n");
    kprintf("  format   - Format filesystem\n");
    kprintf("  edit     - Text editor (edit <file>)\n");
    kprintf("  exec     - Execute ELF binary (exec <path>)\n");
    kprintf("  run      - Execute script file (run <path>)\n");
    kprintf("  ifconfig - Show network info\n");
    kprintf("  ping     - Ping host (ping [ip])\n");
    kprintf("  dns      - Resolve hostname (dns <host>)\n");
    kprintf("  curl     - HTTP GET (curl <url>)\n");
    kprintf("  udpsend  - Send UDP packet (udpsend <ip> <port> <data>)\n");
    kprintf("  beep     - PC speaker beep (beep [freq] [ms])\n");
    kprintf("  play     - Play note sequence (play <note> ...)\n");
    kprintf("  mouse    - Show mouse position and buttons\n");
    kprintf("  wc       - Count lines/words/bytes (wc <file>)\n");
    kprintf("  head     - Show first N lines (head <file> [n])\n");
    kprintf("  tail     - Show last N lines (tail <file> [n])\n");
    kprintf("  cp       - Copy file (cp <src> <dst>)\n");
    kprintf("  mv       - Move file (mv <src> <dst>)\n");
    kprintf("  grep     - Search text in file (grep <pattern> <file>)\n");
    kprintf("  df       - Show disk usage\n");
    kprintf("  free     - Show memory usage\n");
    kprintf("  whoami   - Show current process\n");
    kprintf("  hostname - Show hostname\n");
    kprintf("  env      - Show environment info\n");
    kprintf("  xxd      - Hex dump file (xxd <file>)\n");
    kprintf("  sleep    - Sleep N seconds (sleep <n>)\n");
    kprintf("  seq      - Print number sequence (seq [start] <end>)\n");
    kprintf("  arp      - Show ARP cache\n");
    kprintf("  route    - Show routing table\n");
    kprintf("  uname    - Show system info\n");
    kprintf("  lspci    - List PCI devices\n");
    kprintf("  dmesg    - Show boot log\n");
}

void cmd_echo(const char *args) {
    if (args) kprintf("%s\n", args);
    else kprintf("\n");
}

void cmd_meminfo(void) {
    uint64_t total = pmm_get_total_frames();
    uint64_t used = pmm_get_used_frames();
    uint64_t free_fr = total - used;
    kprintf("Physical memory:\n");
    kprintf("  Total: %u KB (%u frames)\n", total * 4, total);
    kprintf("  Used:  %u KB (%u frames)\n", used * 4, used);
    kprintf("  Free:  %u KB (%u frames)\n", free_fr * 4, free_fr);
}

void cmd_ps(void) {
    extern struct process *process_get_table(void);
    struct process *table = process_get_table();
    const char *state_names[] = { "UNUSED", "READY", "RUNNING", "BLOCKED", "ZOMBIE" };

    kprintf("PID  STATE    NAME\n");
    for (int i = 0; i < PROCESS_MAX; i++) {
        if (table[i].state != PROCESS_UNUSED) {
            kprintf("%-4u %-8s %s\n", (uint64_t)table[i].pid,
                    state_names[table[i].state], table[i].name);
        }
    }
}

void cmd_uptime(void) {
    uint64_t ticks = timer_get_ticks();
    uint64_t seconds = ticks / TIMER_FREQ;
    uint64_t minutes = seconds / 60;
    seconds %= 60;
    kprintf("Uptime: %u min %u sec (%u ticks)\n", minutes, seconds, ticks);
}

void cmd_reboot(void) {
    kprintf("Rebooting...\n");
    struct { uint16_t limit; uint64_t base; } __attribute__((packed)) null_idt = {0, 0};
    __asm__ volatile("lidt %0; int $0" : : "m"(null_idt));
}

void cmd_kill(const char *args) {
    if (!args) { kprintf("Usage: kill <pid> [signal]\n"); return; }
    uint32_t pid = 0;
    while (*args >= '0' && *args <= '9') { pid = pid * 10 + (*args - '0'); args++; }
    while (*args == ' ') args++;
    /* Optional signal number (default SIGKILL=9) */
    int sig = 9;
    if (*args >= '0' && *args <= '9') {
        sig = 0;
        while (*args >= '0' && *args <= '9') { sig = sig * 10 + (*args - '0'); args++; }
    }
    if (pid == 0) { kprintf("Cannot kill pid 0\n"); return; }
    struct process *p = process_get_by_pid(pid);
    if (!p || p->state == PROCESS_UNUSED) { kprintf("No such process: %u\n", (uint64_t)pid); return; }
    if (signal_send(pid, sig) < 0) {
        /* Fallback for SIGKILL when signal subsystem not routed yet */
        p->state = PROCESS_ZOMBIE;
    }
    kprintf("Signal %d sent to process %u (%s)\n", (uint64_t)sig, (uint64_t)pid, p->name);
}

void cmd_color(const char *args) {
    if (!args) { kprintf("Usage: color <fg> [bg] (0-15)\n"); return; }
    uint8_t fg = 0, bg = 0;
    while (*args >= '0' && *args <= '9') { fg = fg * 10 + (*args - '0'); args++; }
    while (*args == ' ') args++;
    if (*args >= '0' && *args <= '9') {
        while (*args >= '0' && *args <= '9') { bg = bg * 10 + (*args - '0'); args++; }
    }
    if (fg > 15) fg = 15;
    if (bg > 15) bg = 15;
    vga_set_color(fg, bg);
    kprintf("Color set to %u on %u\n", (uint64_t)fg, (uint64_t)bg);
}

void cmd_hexdump(const char *args) {
    if (!args) { kprintf("Usage: hexdump <addr> [len]\n"); return; }
    uint64_t addr = 0;
    if (args[0] == '0' && args[1] == 'x') args += 2;
    while (1) {
        char c = *args;
        if (c >= '0' && c <= '9') addr = addr * 16 + (c - '0');
        else if (c >= 'a' && c <= 'f') addr = addr * 16 + (c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') addr = addr * 16 + (c - 'A' + 10);
        else break;
        args++;
    }
    while (*args == ' ') args++;
    uint64_t len = 64;
    if (*args >= '0' && *args <= '9') {
        len = 0;
        while (*args >= '0' && *args <= '9') { len = len * 10 + (*args - '0'); args++; }
    }
    if (len > 256) len = 256;
    uint8_t *ptr = (uint8_t *)addr;
    for (uint64_t i = 0; i < len; i += 16) {
        kprintf("%p: ", (uint64_t)(ptr + i));
        for (int j = 0; j < 16 && i + j < len; j++) kprintf("%x ", (uint64_t)ptr[i + j]);
        kprintf("\n");
    }
}

void cmd_date(void) {
    struct rtc_time t;
    rtc_get_time(&t);
    kprintf("%u-%02u-%02u %02u:%02u:%02u\n",
            (uint64_t)t.year, (uint64_t)t.month, (uint64_t)t.day,
            (uint64_t)t.hour, (uint64_t)t.minute, (uint64_t)t.second);
}

void cmd_cpuinfo(void) {
    uint32_t eax, ebx, ecx, edx;
    char vendor[13];
    __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));
    *(uint32_t*)&vendor[0] = ebx;
    *(uint32_t*)&vendor[4] = edx;
    *(uint32_t*)&vendor[8] = ecx;
    vendor[12] = 0;
    kprintf("CPU Vendor: %s\n", vendor);

    char brand[49];
    memset(brand, 0, 49);
    for (uint32_t i = 0; i < 3; i++) {
        __asm__ volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0x80000002 + i));
        *(uint32_t*)&brand[i*16+0] = eax;
        *(uint32_t*)&brand[i*16+4] = ebx;
        *(uint32_t*)&brand[i*16+8] = ecx;
        *(uint32_t*)&brand[i*16+12] = edx;
    }
    kprintf("CPU Brand:  %s\n", brand);
}

void cmd_ls(const char *args) {
    if (!ata_is_present()) { kprintf("No disk\n"); return; }
    if (fs_list(args ? args : "/") < 0)
        kprintf("Not a directory or not found\n");
}

void cmd_cat(const char *args) {
    if (!args) { kprintf("Usage: cat <file>\n"); return; }
    if (!ata_is_present()) { kprintf("No disk\n"); return; }
    static char fbuf[4096];
    uint32_t size;
    if (fs_read_file(args, fbuf, sizeof(fbuf) - 1, &size) < 0) {
        kprintf("Cannot read file: %s\n", args);
        return;
    }
    fbuf[size] = '\0';
    kprintf("%s\n", fbuf);
}

void cmd_write(const char *args) {
    if (!args) { kprintf("Usage: write <file> <text>\n"); return; }
    if (!ata_is_present()) { kprintf("No disk\n"); return; }
    /* Split filename and content */
    const char *name = args;
    while (*args && *args != ' ') args++;
    if (!*args) { kprintf("Usage: write <file> <text>\n"); return; }
    /* Build path with / prefix if needed */
    char path[64];
    size_t nlen = args - name;
    if (nlen >= sizeof(path) - 2) nlen = sizeof(path) - 2;
    int off = 0;
    if (*name != '/') { path[0] = '/'; off = 1; }
    memcpy(path + off, name, nlen);
    path[off + nlen] = '\0';
    args++; /* skip space */
    if (fs_write_file(path, args, strlen(args)) < 0)
        kprintf("Write failed\n");
    else
        kprintf("Written %u bytes to %s\n", (uint64_t)strlen(args), path);
}

void cmd_touch(const char *args) {
    if (!args) { kprintf("Usage: touch <file>\n"); return; }
    if (!ata_is_present()) { kprintf("No disk\n"); return; }
    char path[64];
    if (args[0] != '/') { path[0] = '/'; strcpy(path + 1, args); }
    else strcpy(path, args);
    if (fs_create(path, FS_TYPE_FILE) < 0)
        kprintf("Cannot create: %s\n", path);
}

void cmd_rm(const char *args) {
    if (!args) { kprintf("Usage: rm <path>\n"); return; }
    if (!ata_is_present()) { kprintf("No disk\n"); return; }
    char path[64];
    if (args[0] != '/') { path[0] = '/'; strcpy(path + 1, args); }
    else strcpy(path, args);
    if (fs_delete(path) < 0)
        kprintf("Cannot remove: %s\n", path);
}

void cmd_mkdir(const char *args) {
    if (!args) { kprintf("Usage: mkdir <dir>\n"); return; }
    if (!ata_is_present()) { kprintf("No disk\n"); return; }
    char path[64];
    if (args[0] != '/') { path[0] = '/'; strcpy(path + 1, args); }
    else strcpy(path, args);
    if (fs_create(path, FS_TYPE_DIR) < 0)
        kprintf("Cannot create directory: %s\n", path);
}

void cmd_stat_file(const char *args) {
    if (!args) { kprintf("Usage: stat <path>\n"); return; }
    if (!ata_is_present()) { kprintf("No disk\n"); return; }
    char path[64];
    if (args[0] != '/') { path[0] = '/'; strcpy(path + 1, args); }
    else strcpy(path, args);
    uint32_t size; uint8_t type;
    if (fs_stat(path, &size, &type) < 0) {
        kprintf("Not found: %s\n", path);
        return;
    }
    kprintf("  Path: %s\n", path);
    kprintf("  Type: %s\n", type == FS_TYPE_DIR ? "directory" : "file");
    kprintf("  Size: %u bytes\n", (uint64_t)size);
}

void cmd_format_disk(void) {
    if (!ata_is_present()) { kprintf("No disk\n"); return; }
    if (fs_format() < 0) kprintf("Format failed\n");
    else kprintf("Filesystem formatted\n");
}

void cmd_history_show(void) {
    shell_history_show_entries();
}

void cmd_ifconfig(void) {
    if (!e1000_is_present()) { kprintf("No network device\n"); return; }
    uint8_t mac[6];
    e1000_get_mac(mac);
    uint8_t ip[4];
    net_get_ip(ip);
    uint8_t gw[4], mask[4];
    uint32_t gw32 = net_get_gateway();
    uint32_t mask32 = net_get_mask();
    gw[0] = (gw32 >> 24) & 0xFF; gw[1] = (gw32 >> 16) & 0xFF;
    gw[2] = (gw32 >> 8) & 0xFF; gw[3] = gw32 & 0xFF;
    mask[0] = (mask32 >> 24) & 0xFF; mask[1] = (mask32 >> 16) & 0xFF;
    mask[2] = (mask32 >> 8) & 0xFF; mask[3] = mask32 & 0xFF;
    kprintf("eth0:\n");
    kprintf("  MAC:  %x:%x:%x:%x:%x:%x\n",
            (uint64_t)mac[0], (uint64_t)mac[1], (uint64_t)mac[2],
            (uint64_t)mac[3], (uint64_t)mac[4], (uint64_t)mac[5]);
    kprintf("  IP:   %u.%u.%u.%u\n",
            (uint64_t)ip[0], (uint64_t)ip[1], (uint64_t)ip[2], (uint64_t)ip[3]);
    kprintf("  Mask: %u.%u.%u.%u\n",
            (uint64_t)mask[0], (uint64_t)mask[1], (uint64_t)mask[2], (uint64_t)mask[3]);
    kprintf("  GW:   %u.%u.%u.%u\n",
            (uint64_t)gw[0], (uint64_t)gw[1], (uint64_t)gw[2], (uint64_t)gw[3]);
}

void cmd_ping(const char *args) {
    if (!e1000_is_present()) { kprintf("No network device\n"); return; }
    uint32_t target;
    if (args && *args) {
        target = net_dns_resolve(args);
        if (!target) { kprintf("Could not resolve %s\n", args); return; }
    } else {
        target = net_get_gateway();
        if (!target) { kprintf("No gateway configured\n"); return; }
    }
    kprintf("PING %u.%u.%u.%u: ",
            (uint64_t)((target >> 24) & 0xFF), (uint64_t)((target >> 16) & 0xFF),
            (uint64_t)((target >> 8) & 0xFF), (uint64_t)(target & 0xFF));
    int ms = net_ping(target);
    if (ms < 0) {
        kprintf("Request timed out\n");
    } else {
        kprintf("Reply in %u ms\n", (uint64_t)ms);
    }
}

void cmd_dns(const char *args) {
    if (!args || !*args) { kprintf("Usage: dns <hostname>\n"); return; }
    if (!e1000_is_present()) { kprintf("No network device\n"); return; }
    kprintf("Resolving %s... ", args);
    uint32_t ip = net_dns_resolve(args);
    if (!ip) {
        kprintf("failed\n");
    } else {
        kprintf("%u.%u.%u.%u\n",
                (uint64_t)((ip >> 24) & 0xFF), (uint64_t)((ip >> 16) & 0xFF),
                (uint64_t)((ip >> 8) & 0xFF), (uint64_t)(ip & 0xFF));
    }
}

void cmd_curl(const char *args) {
    if (!args || !*args) { kprintf("Usage: curl <host>[/path] or curl <host> <port> [path]\n"); return; }
    if (!e1000_is_present()) { kprintf("No network device\n"); return; }

    /* Parse: curl host[/path] or curl host:port[/path] */
    char host[128];
    char path[256];
    uint16_t port = 80;
    int hi = 0;
    const char *p = args;

    /* Extract host */
    while (*p && *p != '/' && *p != ':' && *p != ' ' && hi < 127)
        host[hi++] = *p++;
    host[hi] = '\0';

    /* Check for :port */
    if (*p == ':') {
        p++;
        port = 0;
        while (*p >= '0' && *p <= '9') { port = port * 10 + (*p - '0'); p++; }
    }

    /* Extract path */
    if (*p == '/') {
        int pi = 0;
        while (*p && *p != ' ' && pi < 255)
            path[pi++] = *p++;
        path[pi] = '\0';
    } else {
        path[0] = '/'; path[1] = '\0';
    }

    kprintf("Connecting to %s:%u%s...\n", host, (uint64_t)port, path);

    static char buf[4096];
    int n = net_http_get(host, port, path, buf, sizeof(buf));
    if (n < 0) {
        kprintf("Request failed\n");
    } else {
        kprintf("%s\n", buf);
    }
}

void cmd_shutdown(void) {
    kprintf("Shutting down...\n");
    acpi_shutdown();
}

void cmd_beep(const char *args) {
    uint32_t freq = 1000;
    uint32_t ms   = 200;
    if (args && *args >= '0' && *args <= '9') {
        freq = 0;
        while (*args >= '0' && *args <= '9') { freq = freq * 10 + (*args - '0'); args++; }
        while (*args == ' ') args++;
        if (*args >= '0' && *args <= '9') {
            ms = 0;
            while (*args >= '0' && *args <= '9') { ms = ms * 10 + (*args - '0'); args++; }
        }
    }
    speaker_beep(freq, ms);
}

static uint32_t note_freq(const char *note) {
    if (strcmp(note, "C4") == 0) return NOTE_C4;
    if (strcmp(note, "D4") == 0) return NOTE_D4;
    if (strcmp(note, "E4") == 0) return NOTE_E4;
    if (strcmp(note, "F4") == 0) return NOTE_F4;
    if (strcmp(note, "G4") == 0) return NOTE_G4;
    if (strcmp(note, "A4") == 0) return NOTE_A4;
    if (strcmp(note, "B4") == 0) return NOTE_B4;
    if (strcmp(note, "C5") == 0) return NOTE_C5;
    return 440;
}

void cmd_play(const char *args) {
    if (!args || !*args) { kprintf("Usage: play <note> [note ...]\n"); return; }
    char note[8];
    while (*args) {
        while (*args == ' ') args++;
        if (!*args) break;
        int ni = 0;
        while (*args && *args != ' ' && ni < 7) note[ni++] = *args++;
        note[ni] = '\0';
        speaker_beep(note_freq(note), 200);
        uint64_t start = timer_get_ticks();
        while (timer_get_ticks() - start < (uint64_t)TIMER_FREQ / 20);
    }
}

void cmd_mouse_status(void) {
    int x, y;
    mouse_get_pos(&x, &y);
    uint8_t btn = mouse_get_buttons();
    kprintf("Mouse: x=%d y=%d buttons=0x%x (L=%d M=%d R=%d)\n",
            (uint64_t)x, (uint64_t)y, (uint64_t)btn,
            (uint64_t)(btn & 1), (uint64_t)((btn >> 2) & 1), (uint64_t)((btn >> 1) & 1));
}

void cmd_udpsend(const char *args) {
    if (!args || !*args) { kprintf("Usage: udpsend <ip> <port> <data>\n"); return; }
    if (!e1000_is_present()) { kprintf("No network device\n"); return; }

    char ipstr[20];
    int ii = 0;
    const char *p = args;
    while (*p && *p != ' ' && ii < 19) ipstr[ii++] = *p++;
    ipstr[ii] = '\0';
    while (*p == ' ') p++;

    uint32_t parts[4] = {0};
    int part = 0;
    for (int i = 0; ipstr[i] && part < 4; i++) {
        if (ipstr[i] >= '0' && ipstr[i] <= '9') parts[part] = parts[part] * 10 + (uint32_t)(ipstr[i] - '0');
        else if (ipstr[i] == '.') part++;
    }
    uint32_t dst_ip = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];

    uint16_t port = 0;
    while (*p >= '0' && *p <= '9') { port = (uint16_t)(port * 10 + (*p - '0')); p++; }
    while (*p == ' ') p++;

    if (!*p) { kprintf("Usage: udpsend <ip> <port> <data>\n"); return; }

    net_udp_send(dst_ip, 12345, port, p, strlen(p));
    kprintf("UDP sent %u bytes to %u.%u.%u.%u:%u\n",
            (uint64_t)strlen(p),
            (uint64_t)((dst_ip >> 24) & 0xFF), (uint64_t)((dst_ip >> 16) & 0xFF),
            (uint64_t)((dst_ip >> 8) & 0xFF), (uint64_t)(dst_ip & 0xFF),
            (uint64_t)port);
}

void cmd_exec(const char *args) {
    if (!args || !*args) { kprintf("Usage: exec <path>\n"); return; }
    if (!ata_is_present()) { kprintf("No disk\n"); return; }
    char path[64];
    if (args[0] != '/') { path[0] = '/'; strcpy(path + 1, args); }
    else strcpy(path, args);
    elf_exec(path);
}

void cmd_run(const char *args) {
    if (!args || !*args) { kprintf("Usage: run <script>\n"); return; }
    if (!ata_is_present()) { kprintf("No disk\n"); return; }
    char path[64];
    if (args[0] != '/') { path[0] = '/'; strcpy(path + 1, args); }
    else strcpy(path, args);
    script_exec(path);
}

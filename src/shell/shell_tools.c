/* shell_tools.c — Additional tool command implementations */

#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "timer.h"
#include "pmm.h"
#include "process.h"
#include "scheduler.h"
#include "fs.h"
#include "ata.h"
#include "net.h"
#include "e1000.h"
#include "pci.h"

static uint32_t parse_uint(const char **s) {
    uint32_t v = 0;
    while (**s >= '0' && **s <= '9') { v = v * 10 + (**s - '0'); (*s)++; }
    return v;
}

void cmd_wc(const char *args) {
    if (!args) { kprintf("Usage: wc <file>\n"); return; }
    if (!ata_is_present()) { kprintf("No disk\n"); return; }
    char path[64];
    if (args[0] != '/') { path[0] = '/'; strcpy(path + 1, args); }
    else strcpy(path, args);
    static char fbuf[4096];
    uint32_t size;
    if (fs_read_file(path, fbuf, sizeof(fbuf) - 1, &size) < 0) {
        kprintf("Cannot read: %s\n", path);
        return;
    }
    fbuf[size] = '\0';
    uint32_t lines = 0, words = 0;
    int in_word = 0;
    for (uint32_t i = 0; i < size; i++) {
        if (fbuf[i] == '\n') lines++;
        if (fbuf[i] == ' ' || fbuf[i] == '\n' || fbuf[i] == '\t') {
            in_word = 0;
        } else if (!in_word) {
            in_word = 1;
            words++;
        }
    }
    kprintf("  %u %u %u %s\n", (uint64_t)lines, (uint64_t)words,
            (uint64_t)size, args);
}

void cmd_head(const char *args) {
    if (!args) { kprintf("Usage: head <file> [n]\n"); return; }
    if (!ata_is_present()) { kprintf("No disk\n"); return; }
    const char *p = args;
    char name[64];
    int ni = 0;
    while (*p && *p != ' ' && ni < 63) name[ni++] = *p++;
    name[ni] = '\0';
    while (*p == ' ') p++;
    uint32_t n = 10;
    if (*p >= '0' && *p <= '9') n = parse_uint(&p);
    char path[64];
    if (name[0] != '/') { path[0] = '/'; strcpy(path + 1, name); }
    else strcpy(path, name);
    static char fbuf[4096];
    uint32_t size;
    if (fs_read_file(path, fbuf, sizeof(fbuf) - 1, &size) < 0) {
        kprintf("Cannot read: %s\n", name);
        return;
    }
    fbuf[size] = '\0';
    uint32_t line = 0;
    for (uint32_t i = 0; i < size && line < n; i++) {
        kprintf("%c", (uint64_t)(uint8_t)fbuf[i]);
        if (fbuf[i] == '\n') line++;
    }
    if (size > 0 && fbuf[size - 1] != '\n') kprintf("\n");
}

void cmd_tail(const char *args) {
    if (!args) { kprintf("Usage: tail <file> [n]\n"); return; }
    if (!ata_is_present()) { kprintf("No disk\n"); return; }
    const char *p = args;
    char name[64];
    int ni = 0;
    while (*p && *p != ' ' && ni < 63) name[ni++] = *p++;
    name[ni] = '\0';
    while (*p == ' ') p++;
    uint32_t n = 10;
    if (*p >= '0' && *p <= '9') n = parse_uint(&p);
    char path[64];
    if (name[0] != '/') { path[0] = '/'; strcpy(path + 1, name); }
    else strcpy(path, name);
    static char fbuf[4096];
    uint32_t size;
    if (fs_read_file(path, fbuf, sizeof(fbuf) - 1, &size) < 0) {
        kprintf("Cannot read: %s\n", name);
        return;
    }
    fbuf[size] = '\0';
    /* Count total lines */
    uint32_t total_lines = 0;
    for (uint32_t i = 0; i < size; i++)
        if (fbuf[i] == '\n') total_lines++;
    uint32_t skip = (total_lines > n) ? (total_lines - n) : 0;
    uint32_t line = 0;
    for (uint32_t i = 0; i < size; i++) {
        if (line >= skip) kprintf("%c", (uint64_t)(uint8_t)fbuf[i]);
        if (fbuf[i] == '\n') line++;
    }
    if (size > 0 && fbuf[size - 1] != '\n') kprintf("\n");
}

void cmd_cp(const char *args) {
    if (!args) { kprintf("Usage: cp <src> <dst>\n"); return; }
    if (!ata_is_present()) { kprintf("No disk\n"); return; }
    const char *p = args;
    char src[64], dst[64];
    int si = 0;
    while (*p && *p != ' ' && si < 63) src[si++] = *p++;
    src[si] = '\0';
    while (*p == ' ') p++;
    int di = 0;
    while (*p && *p != ' ' && di < 63) dst[di++] = *p++;
    dst[di] = '\0';
    if (!dst[0]) { kprintf("Usage: cp <src> <dst>\n"); return; }
    char spath[64], dpath[64];
    if (src[0] != '/') { spath[0] = '/'; strcpy(spath + 1, src); }
    else strcpy(spath, src);
    if (dst[0] != '/') { dpath[0] = '/'; strcpy(dpath + 1, dst); }
    else strcpy(dpath, dst);
    static char fbuf[4096];
    uint32_t size;
    if (fs_read_file(spath, fbuf, sizeof(fbuf), &size) < 0) {
        kprintf("Cannot read: %s\n", src);
        return;
    }
    /* Create dest if needed */
    uint32_t ds; uint8_t dt;
    if (fs_stat(dpath, &ds, &dt) < 0) {
        if (fs_create(dpath, FS_TYPE_FILE) < 0) {
            kprintf("Cannot create: %s\n", dst);
            return;
        }
    }
    if (fs_write_file(dpath, fbuf, size) < 0) {
        kprintf("Write failed: %s\n", dst);
        return;
    }
    kprintf("Copied %u bytes: %s -> %s\n", (uint64_t)size, src, dst);
}

void cmd_mv(const char *args) {
    if (!args) { kprintf("Usage: mv <src> <dst>\n"); return; }
    if (!ata_is_present()) { kprintf("No disk\n"); return; }
    const char *p = args;
    char src[64], dst[64];
    int si = 0;
    while (*p && *p != ' ' && si < 63) src[si++] = *p++;
    src[si] = '\0';
    while (*p == ' ') p++;
    int di = 0;
    while (*p && *p != ' ' && di < 63) dst[di++] = *p++;
    dst[di] = '\0';
    if (!dst[0]) { kprintf("Usage: mv <src> <dst>\n"); return; }
    char spath[64], dpath[64];
    if (src[0] != '/') { spath[0] = '/'; strcpy(spath + 1, src); }
    else strcpy(spath, src);
    if (dst[0] != '/') { dpath[0] = '/'; strcpy(dpath + 1, dst); }
    else strcpy(dpath, dst);
    static char fbuf[4096];
    uint32_t size;
    if (fs_read_file(spath, fbuf, sizeof(fbuf), &size) < 0) {
        kprintf("Cannot read: %s\n", src);
        return;
    }
    uint32_t ds; uint8_t dt;
    if (fs_stat(dpath, &ds, &dt) < 0) {
        if (fs_create(dpath, FS_TYPE_FILE) < 0) {
            kprintf("Cannot create: %s\n", dst);
            return;
        }
    }
    if (fs_write_file(dpath, fbuf, size) < 0) {
        kprintf("Write failed: %s\n", dst);
        return;
    }
    fs_delete(spath);
    kprintf("Moved: %s -> %s\n", src, dst);
}

void cmd_grep(const char *args) {
    if (!args) { kprintf("Usage: grep <pattern> <file>\n"); return; }
    if (!ata_is_present()) { kprintf("No disk\n"); return; }
    const char *p = args;
    char pattern[128];
    int pi = 0;
    while (*p && *p != ' ' && pi < 127) pattern[pi++] = *p++;
    pattern[pi] = '\0';
    while (*p == ' ') p++;
    if (!*p) { kprintf("Usage: grep <pattern> <file>\n"); return; }
    char path[64];
    if (*p != '/') { path[0] = '/'; strcpy(path + 1, p); }
    else strcpy(path, p);
    /* Trim trailing spaces from path */
    int pl = strlen(path);
    while (pl > 0 && path[pl - 1] == ' ') path[--pl] = '\0';
    static char fbuf[4096];
    uint32_t size;
    if (fs_read_file(path, fbuf, sizeof(fbuf) - 1, &size) < 0) {
        kprintf("Cannot read: %s\n", p);
        return;
    }
    fbuf[size] = '\0';
    int plen = strlen(pattern);
    /* Print lines containing pattern */
    char *line = fbuf;
    int count = 0;
    for (uint32_t i = 0; i <= size; i++) {
        if (fbuf[i] == '\n' || i == size) {
            fbuf[i] = '\0';
            /* Simple substring search */
            int found = 0;
            for (char *s = line; *s; s++) {
                if (strncmp(s, pattern, plen) == 0) { found = 1; break; }
            }
            if (found) {
                kprintf("%s\n", line);
                count++;
            }
            line = &fbuf[i + 1];
        }
    }
    if (count == 0) kprintf("No matches\n");
}

void cmd_df(void) {
    if (!ata_is_present()) { kprintf("No disk\n"); return; }
    uint32_t total_sectors = ata_get_sectors();
    uint32_t used_inodes, total_inodes, used_blocks, data_start;
    fs_get_usage(&used_inodes, &total_inodes, &used_blocks, &data_start);
    uint32_t avail = total_sectors > (data_start + used_blocks)
                     ? total_sectors - data_start - used_blocks : 0;
    kprintf("Filesystem      Blocks  Used    Avail   Inodes\n");
    kprintf("/dev/hda        %-7u %-7u %-7u %u/%u\n",
            (uint64_t)(total_sectors - data_start),
            (uint64_t)used_blocks,
            (uint64_t)avail,
            (uint64_t)used_inodes,
            (uint64_t)total_inodes);
}

void cmd_free(void) {
    uint64_t total = pmm_get_total_frames();
    uint64_t used = pmm_get_used_frames();
    uint64_t free_fr = total - used;
    kprintf("              total      used       free\n");
    kprintf("Mem:     %9u %9u  %9u  KB\n",
            total * 4, used * 4, free_fr * 4);
    kprintf("Frames:  %9u %9u  %9u\n",
            total, used, free_fr);
}

void cmd_whoami(void) {
    struct process *p = process_get_current();
    kprintf("PID %u (%s)\n", (uint64_t)p->pid, p->name);
}

void cmd_hostname(void) {
    kprintf("os-kernel\n");
}

void cmd_env(void) {
    uint8_t ip[4];
    net_get_ip(ip);
    uint64_t ticks = timer_get_ticks();
    uint64_t sec = ticks / TIMER_FREQ;
    struct process *p = process_get_current();
    kprintf("PID=%u\n", (uint64_t)p->pid);
    kprintf("NAME=%s\n", p->name);
    kprintf("UPTIME=%u\n", sec);
    kprintf("IP=%u.%u.%u.%u\n",
            (uint64_t)ip[0], (uint64_t)ip[1],
            (uint64_t)ip[2], (uint64_t)ip[3]);
    kprintf("HOSTNAME=os-kernel\n");
    kprintf("DISK=%s\n", ata_is_present() ? "yes" : "no");
    kprintf("NET=%s\n", e1000_is_present() ? "yes" : "no");
}

void cmd_xxd(const char *args) {
    if (!args) { kprintf("Usage: xxd <file>\n"); return; }
    if (!ata_is_present()) { kprintf("No disk\n"); return; }
    char path[64];
    if (args[0] != '/') { path[0] = '/'; strcpy(path + 1, args); }
    else strcpy(path, args);
    static char fbuf[4096];
    uint32_t size;
    if (fs_read_file(path, fbuf, sizeof(fbuf), &size) < 0) {
        kprintf("Cannot read: %s\n", args);
        return;
    }
    if (size > 256) size = 256;
    for (uint32_t i = 0; i < size; i += 16) {
        kprintf("%08x: ", (uint64_t)i);
        for (int j = 0; j < 16; j++) {
            if (i + j < size)
                kprintf("%02x ", (uint64_t)(uint8_t)fbuf[i + j]);
            else
                kprintf("   ");
        }
        kprintf(" ");
        for (int j = 0; j < 16 && i + j < size; j++) {
            char c = fbuf[i + j];
            kprintf("%c", (uint64_t)(uint8_t)((c >= 32 && c < 127) ? c : '.'));
        }
        kprintf("\n");
    }
}

void cmd_sleep(const char *args) {
    if (!args || !(*args >= '0' && *args <= '9')) {
        kprintf("Usage: sleep <seconds>\n");
        return;
    }
    const char *p = args;
    uint32_t sec = parse_uint(&p);
    if (sec > 60) sec = 60;
    uint64_t target = timer_get_ticks() + (uint64_t)sec * TIMER_FREQ;
    while (timer_get_ticks() < target)
        scheduler_yield();
    kprintf("Slept %u seconds\n", (uint64_t)sec);
}

void cmd_seq(const char *args) {
    if (!args) { kprintf("Usage: seq <end> or seq <start> <end>\n"); return; }
    const char *p = args;
    uint32_t a = parse_uint(&p);
    while (*p == ' ') p++;
    uint32_t start = 1, end = a;
    if (*p >= '0' && *p <= '9') {
        start = a;
        end = parse_uint(&p);
    }
    if (end > 1000) end = 1000;
    for (uint32_t i = start; i <= end; i++)
        kprintf("%u\n", (uint64_t)i);
}

static void arp_print_entry(uint32_t ip, const uint8_t *mac) {
    kprintf("  %u.%u.%u.%u  ->  %x:%x:%x:%x:%x:%x\n",
            (uint64_t)((ip >> 24) & 0xFF), (uint64_t)((ip >> 16) & 0xFF),
            (uint64_t)((ip >> 8) & 0xFF), (uint64_t)(ip & 0xFF),
            (uint64_t)mac[0], (uint64_t)mac[1], (uint64_t)mac[2],
            (uint64_t)mac[3], (uint64_t)mac[4], (uint64_t)mac[5]);
}

void cmd_arp(void) {
    kprintf("ARP cache:\n");
    int n = net_arp_list(arp_print_entry);
    if (n == 0) kprintf("  (empty)\n");
    kprintf("Entries: %u\n", (uint64_t)n);
}

void cmd_route(void) {
    uint32_t gw = net_get_gateway();
    uint32_t mask = net_get_mask();
    uint8_t ip[4];
    net_get_ip(ip);
    kprintf("Routing table:\n");
    kprintf("Destination     Gateway         Mask            Iface\n");
    kprintf("%-15s %-15s %u.%u.%u.%u   eth0\n",
            "0.0.0.0",
            "0.0.0.0",
            (uint64_t)((mask >> 24) & 0xFF), (uint64_t)((mask >> 16) & 0xFF),
            (uint64_t)((mask >> 8) & 0xFF), (uint64_t)(mask & 0xFF));
    kprintf("%-15s %u.%u.%u.%u  %u.%u.%u.%u   eth0\n",
            "default",
            (uint64_t)((gw >> 24) & 0xFF), (uint64_t)((gw >> 16) & 0xFF),
            (uint64_t)((gw >> 8) & 0xFF), (uint64_t)(gw & 0xFF),
            (uint64_t)((mask >> 24) & 0xFF), (uint64_t)((mask >> 16) & 0xFF),
            (uint64_t)((mask >> 8) & 0xFF), (uint64_t)(mask & 0xFF));
}

void cmd_uname(void) {
    kprintf("OS kernel x86_64 v0.1\n");
}

void cmd_lspci(void) {
    pci_list();
}

void cmd_dmesg(void) {
    kprintf("Boot log not captured. Use serial output.\n");
}

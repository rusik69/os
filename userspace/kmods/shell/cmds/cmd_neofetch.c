/* cmd_neofetch.c — neofetch: display system information with ASCII art */

#include "shell_cmds.h"
#include "printf.h"
#include "string.h"
#include "vga.h"
#include "libc.h"

/* ── Local helpers ───────────────────────────────────────────────────── */

/* Read CPU vendor string via CPUID leaf 0 */
static void cpu_get_vendor(char *vendor, int max_len)
{
    uint32_t ebx, ecx, edx;
    __asm__ volatile("cpuid" : "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(0));
    if (max_len > 12) max_len = 12;
    char buf[13];
    memcpy(buf,      &ebx, 4);
    memcpy(buf + 4,  &edx, 4);
    memcpy(buf + 8,  &ecx, 4);
    buf[12] = '\0';
    strncpy(vendor, buf, (size_t)max_len - 1);
    vendor[max_len - 1] = '\0';
}

/* Read CPU brand string via extended CPUID leaves */
static void cpu_get_brand(char *brand, int max_len)
{
    uint32_t eax, ebx, ecx, edx;
    char tmp[49];
    memset(tmp, 0, sizeof(tmp));

    for (uint32_t leaf = 0x80000002; leaf <= 0x80000004; leaf++) {
        __asm__ volatile("cpuid"
            : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(leaf));
        int off = (int)((leaf - 0x80000002) * 16);
        memcpy(tmp + off,      &eax, 4);
        memcpy(tmp + off + 4,  &ebx, 4);
        memcpy(tmp + off + 8,  &ecx, 4);
        memcpy(tmp + off + 12, &edx, 4);
    }
    tmp[48] = '\0';

    /* Remove trailing spaces */
    int end = (int)strlen(tmp) - 1;
    while (end >= 0 && tmp[end] == ' ') tmp[end--] = '\0';

    if (max_len > 48) max_len = 48;
    strncpy(brand, tmp, (size_t)max_len - 1);
    brand[max_len - 1] = '\0';
}

/* Get logical CPU count via CPUID leaf 1 */
static int cpu_get_count(void)
{
    uint32_t ebx;
    __asm__ volatile("cpuid" : "=b"(ebx) : "a"(1) : "ecx", "edx");
    int logical = (int)((ebx >> 16) & 0xFF);
    return logical > 0 ? logical : 1;
}

/* Format uptime string: "X days, HH:MM:SS" */
static void format_uptime(uint64_t seconds, char *buf, int max_len)
{
    uint64_t days  = seconds / 86400;
    seconds %= 86400;
    uint64_t hours = seconds / 3600;
    seconds %= 3600;
    uint64_t mins  = seconds / 60;
    seconds %= 60;

    if (days > 0)
        snprintf(buf, (size_t)max_len, "%llu days, %02llu:%02llu:%02llu",
                 (unsigned long long)days,
                 (unsigned long long)hours,
                 (unsigned long long)mins,
                 (unsigned long long)seconds);
    else
        snprintf(buf, (size_t)max_len, "%02llu:%02llu:%02llu",
                 (unsigned long long)hours,
                 (unsigned long long)mins,
                 (unsigned long long)seconds);
}

/* Count processes via the libc process list API */
static int count_processes(void)
{
    struct libc_process_info info;
    int count = 0;
    while (count < PROCESS_MAX) {
        if (libc_process_list(&info, 1) <= 0)
            break;
        count++;
    }
    return count;
}

/* ── The neofetch command ────────────────────────────────────────────── */

void cmd_neofetch(void)
{
    char vendor[16], brand[64];
    char uptime_str[32];
    struct libc_pmm_stats mem_stats;

    /* Gather system info */
    cpu_get_vendor(vendor, sizeof(vendor));
    cpu_get_brand(brand, sizeof(brand));

    uint64_t ticks = libc_uptime_ticks();
    uint64_t seconds = ticks / TIMER_FREQ;
    format_uptime(seconds, uptime_str, sizeof(uptime_str));

    memset(&mem_stats, 0, sizeof(mem_stats));
    pmm_get_stats(&mem_stats);

    int cpu_count = cpu_get_count();
    int proc_count = count_processes();

    /* ── Colorful ASCII art + system info ─────────────────────────── */
    vga_set_color(VGA_CYAN, VGA_BLACK);
    kprintf("        ██████\n");
    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    kprintf("      ██████████\n");
    vga_set_color(VGA_CYAN, VGA_BLACK);
    kprintf("    ██████████████        ");

    /* Line 1: OS and Kernel */
    vga_set_color(VGA_WHITE, VGA_BLACK);
    kprintf("OS:       ");
    vga_set_color(VGA_YELLOW, VGA_BLACK);
    kprintf("HermesOS\n");

    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    kprintf("  ██████████████████      ");
    vga_set_color(VGA_WHITE, VGA_BLACK);
    kprintf("Host:     ");
    vga_set_color(VGA_YELLOW, VGA_BLACK);
    kprintf("x86-64\n");

    vga_set_color(VGA_CYAN, VGA_BLACK);
    kprintf("  ██████████████████      ");
    vga_set_color(VGA_WHITE, VGA_BLACK);
    kprintf("Kernel:   ");
    vga_set_color(VGA_YELLOW, VGA_BLACK);
    kprintf("Hermes v1.0\n");

    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    kprintf("    ██████  ██████        ");
    vga_set_color(VGA_WHITE, VGA_BLACK);
    kprintf("Uptime:   ");
    vga_set_color(VGA_GREEN, VGA_BLACK);
    kprintf("%s\n", uptime_str);

    vga_set_color(VGA_CYAN, VGA_BLACK);
    kprintf("    ████      ████        ");
    vga_set_color(VGA_WHITE, VGA_BLACK);
    kprintf("Packages: ");
    vga_set_color(VGA_GREEN, VGA_BLACK);
    kprintf("N/A (built-in)\n");

    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    kprintf("  ██████████████████      ");
    vga_set_color(VGA_WHITE, VGA_BLACK);
    kprintf("Shell:    ");
    vga_set_color(VGA_GREEN, VGA_BLACK);
    kprintf("built-in\n");

    vga_set_color(VGA_CYAN, VGA_BLACK);
    kprintf("  ██████████████████      ");
    vga_set_color(VGA_WHITE, VGA_BLACK);
    kprintf("CPU:      ");
    vga_set_color(VGA_GREEN, VGA_BLACK);
    kprintf("%s\n", vendor);

    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    kprintf("    ██████████████        ");
    vga_set_color(VGA_WHITE, VGA_BLACK);
    kprintf("Cores:    ");
    vga_set_color(VGA_GREEN, VGA_BLACK);
    kprintf("%d\n", cpu_count);

    vga_set_color(VGA_CYAN, VGA_BLACK);
    kprintf("      ██████████          ");
    vga_set_color(VGA_WHITE, VGA_BLACK);
    kprintf("Memory:   ");
    vga_set_color(VGA_GREEN, VGA_BLACK);
    {
        uint64_t total_kb = (uint64_t)mem_stats.total_pages * 4;
        uint64_t used_kb  = (uint64_t)mem_stats.used_pages * 4;
        uint64_t free_kb  = (uint64_t)mem_stats.free_pages * 4;
        kprintf("%llu KB / %llu KB", (unsigned long long)used_kb,
                (unsigned long long)total_kb);
        vga_set_color(VGA_WHITE, VGA_BLACK);
        kprintf(" (");
        vga_set_color(VGA_GREEN, VGA_BLACK);
        kprintf("%llu KB free", (unsigned long long)free_kb);
        vga_set_color(VGA_WHITE, VGA_BLACK);
        kprintf(")\n");
    }

    vga_set_color(VGA_LIGHT_CYAN, VGA_BLACK);
    kprintf("        ██████            ");
    vga_set_color(VGA_WHITE, VGA_BLACK);
    kprintf("Processes:");
    vga_set_color(VGA_GREEN, VGA_BLACK);
    kprintf(" %d\n", proc_count);

    /* CPU brand on its own line (may be long) */
    if (brand[0]) {
        vga_set_color(VGA_CYAN, VGA_BLACK);
        kprintf("                           ");
        vga_set_color(VGA_GREEN, VGA_BLACK);
        kprintf("CPU Model: %s\n", brand);
    }

    /* Restore default colors */
    vga_set_color(VGA_WHITE, VGA_BLACK);
}

/* cmd_top.c — htop-like interactive process viewer (Item U46)
 *
 * Features:
 *   - Real-time process list with sortable columns
 *   - CPU% and memory usage visualization
 *   - Kill processes with 'k' key
 *   - Sort by PID, name, CPU, state, priority
 *   - Color-coded process states
 *   - System summary header
 */

#include "shell_cmds.h"
#include "printf.h"
#include "vga.h"
#include "libc.h"
#include "keyboard.h"
#include "string.h"
#include "vga.h"

/* ── Simple integer parser (no libc dep) ────────────────────────── */
static int parse_int(const char *s)
{
    if (!s || !*s) return 0;
    int sign = 1, val = 0;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9')
        val = val * 10 + (*s++ - '0');
    return sign * val;
}

/* ── Sort modes ──────────────────────────────────────────────────── */
enum htop_sort {
    HTOP_SORT_PID,
    HTOP_SORT_NAME,
    HTOP_SORT_CPU,
    HTOP_SORT_STATE,
    HTOP_SORT_PRIO,
    HTOP_SORT_NICE,
};

/* ── Comparator helpers ──────────────────────────────────────────── */
static int cmp_pid(const struct libc_process_info *a,
                   const struct libc_process_info *b)
{
    if (a->pid < b->pid) return -1;
    if (a->pid > b->pid) return  1;
    return 0;
}

static int cmp_name(const struct libc_process_info *a,
                    const struct libc_process_info *b)
{
    int r = strcmp(a->name, b->name);
    if (r != 0) return r;
    return cmp_pid(a, b);
}

static int cmp_cpu(const struct libc_process_info *a,
                   const struct libc_process_info *b)
{
    uint64_t total_a = a->cpu_user_ticks + a->cpu_system_ticks;
    uint64_t total_b = b->cpu_user_ticks + b->cpu_system_ticks;
    if (total_b < total_a) return -1; /* descending */
    if (total_b > total_a) return  1;
    return cmp_pid(a, b);
}

static int cmp_state(const struct libc_process_info *a,
                     const struct libc_process_info *b)
{
    if (a->state < b->state) return -1;
    if (a->state > b->state) return  1;
    return cmp_pid(a, b);
}

static int cmp_prio(const struct libc_process_info *a,
                    const struct libc_process_info *b)
{
    if (a->priority < b->priority) return -1;
    if (a->priority > b->priority) return  1;
    return cmp_pid(a, b);
}

static int cmp_nice(const struct libc_process_info *a,
                    const struct libc_process_info *b)
{
    if (a->nice < b->nice) return -1;
    if (a->nice > b->nice) return  1;
    return cmp_pid(a, b);
}

/* ── Sort dispatcher ─────────────────────────────────────────────── */
static void sort_procs(struct libc_process_info *procs, int n,
                       enum htop_sort mode)
{
    /* Simple insertion sort — adequate for <256 processes */
    for (int i = 1; i < n; i++) {
        struct libc_process_info key = procs[i];
        int j = i - 1;
        int move = 0;

        while (j >= 0) {
            int cmp = 0;
            switch (mode) {
            case HTOP_SORT_PID:   cmp = cmp_pid(&procs[j], &key);   break;
            case HTOP_SORT_NAME:  cmp = cmp_name(&procs[j], &key);  break;
            case HTOP_SORT_CPU:   cmp = cmp_cpu(&procs[j], &key);   break;
            case HTOP_SORT_STATE: cmp = cmp_state(&procs[j], &key); break;
            case HTOP_SORT_PRIO:  cmp = cmp_prio(&procs[j], &key);  break;
            case HTOP_SORT_NICE:  cmp = cmp_nice(&procs[j], &key);  break;
            }
            if (cmp <= 0) break;
            procs[j + 1] = procs[j];
            j--;
            move = 1;
        }
        if (move || j + 1 != i)
            procs[j + 1] = key;
    }
}

/* ── State color ─────────────────────────────────────────────────── */
static uint8_t state_color(uint8_t st)
{
    switch (st) {
    case 2: return VGA_GREEN;       /* RUNNING */
    case 1: return VGA_CYAN;        /* READY */
    case 0: return VGA_DARK_GREY;   /* UNUSED */
    case 3: return VGA_RED;         /* BLOCKED */
    case 4: return VGA_BROWN;       /* ZOMBIE */
    default: return VGA_WHITE;
    }
}

static const char *state_name(uint8_t st)
{
    switch (st) {
    case 0: return "UNUSED ";
    case 1: return "READY  ";
    case 2: return "RUNNING";
    case 3: return "BLOCKED";
    case 4: return "ZOMBIE ";
    default: return "?      ";
    }
}

/* ── Main htop loop ──────────────────────────────────────────────── */
void cmd_top(void)
{
    struct libc_process_info procs[PROCESS_MAX];
    enum htop_sort sort_mode = HTOP_SORT_CPU;
    int running = 1;

    kprintf("htop — interactive process viewer. "
            "'q'=quit 'k'=kill 'p'=sort-PID 'n'=sort-name 'c'=sort-CPU "
            "'s'=sort-state 'r'=sort-prio 'i'=sort-nice\n");

    while (running) {
        /* ── Gather processes ─────────────────────────────────── */
        int n = libc_process_list(procs, PROCESS_MAX);
        if (n < 0) n = 0;

        /* Sort */
        sort_procs(procs, n, sort_mode);

        /* ── Draw header ──────────────────────────────────────── */
        vga_clear();
        vga_set_cursor(0, 0);

        vga_set_color(VGA_BLACK, VGA_WHITE);
        kprintf("htop ");
        vga_set_color(VGA_WHITE, VGA_BLACK);

        uint64_t uptime = libc_uptime_ticks() / TIMER_FREQ;
        uint64_t uptime_s = uptime % 60;
        uint64_t uptime_m = (uptime / 60) % 60;
        uint64_t uptime_h = uptime / 3600;

        /* Count running/blocked */
        int running_cnt = 0, blocked_cnt = 0, zombie_cnt = 0, ready_cnt = 0;
        for (int i = 0; i < n; i++) {
            switch (procs[i].state) {
            case 1: ready_cnt++;   break;
            case 2: running_cnt++; break;
            case 3: blocked_cnt++; break;
            case 4: zombie_cnt++;  break;
            default: break;
            }
        }

        kprintf("  Up: %lluh %02llum %02llus  "
                "Tasks: %d  Running: %d  Ready: %d  "
                "Blocked: %d  Zombie: %d\n",
                (unsigned long long)uptime_h,
                (unsigned long long)uptime_m,
                (unsigned long long)uptime_s,
                n, running_cnt, ready_cnt, blocked_cnt, zombie_cnt);

        /* ── Header line ──────────────────────────────────────── */
        vga_set_color(VGA_CYAN, VGA_BLACK);
        kprintf("%-6s %-6s %-10s %-3s %-4s %-7s %-8s  %s\n",
                "PID", "PPID", "STATE", "PRI", "NICE", "CPU%", "MEM",
                "NAME");
        vga_set_color(VGA_WHITE, VGA_BLACK);

        /* ── Find max CPU time for relative percentage ────────── */
        uint64_t max_cpu = 0;
        for (int i = 0; i < n; i++) {
            uint64_t total = procs[i].cpu_user_ticks
                           + procs[i].cpu_system_ticks;
            if (total > max_cpu) max_cpu = total;
        }

        /* ── Draw process lines ──────────────────────────────── */
        int rows_avail = VGA_HEIGHT - 4;  /* header + separator + status */
        int display_end = n < rows_avail ? n : rows_avail;

        for (int i = 0; i < display_end; i++) {
            struct libc_process_info *p = &procs[i];

            /* State color */
            vga_set_color(state_color(p->state), VGA_BLACK);
            kprintf("%-6s ", state_name(p->state));
            vga_set_color(VGA_WHITE, VGA_BLACK);

            /* PID, PPID */
            kprintf("%-6u %-6u ",
                    (unsigned int)p->pid,
                    (unsigned int)p->ppid);

            /* Priority and nice */
            kprintf("%-3u %-4d ",
                    (unsigned int)p->priority,
                    p->nice);

            /* CPU%: compute as (this process ticks / max ticks) * 100 */
            uint64_t total_ticks = p->cpu_user_ticks + p->cpu_system_ticks;
            int cpu_pct = 0;
            if (max_cpu > 0) {
                cpu_pct = (int)((total_ticks * 100ULL) / max_cpu);
                if (cpu_pct > 99) cpu_pct = 99;
            }

            /* Color-code CPU% */
            if (cpu_pct > 50)
                vga_set_color(VGA_RED, VGA_BLACK);
            else if (cpu_pct > 20)
                vga_set_color(VGA_BROWN, VGA_BLACK);
            else
                vga_set_color(VGA_WHITE, VGA_BLACK);

            kprintf(" %2d%%  ", cpu_pct);

            /* Memory bar (using max_rss as proxy) */
            vga_set_color(VGA_GREEN, VGA_BLACK);
            if (p->max_rss > 0) {
                /* Show mem pages as a count */
                kprintf("%-4llu ", (unsigned long long)p->max_rss);
            } else {
                kprintf("0    ");
            }
            vga_set_color(VGA_WHITE, VGA_BLACK);

            /* Name (truncate to fit) */
            int name_len = (int)strlen(p->name);
            if (name_len > 24) name_len = 24;
            kprintf("%.*s", name_len, p->name);

            /* Scroll if needed */
            if (i == rows_avail - 1 && n > rows_avail) {
                vga_set_color(VGA_DARK_GREY, VGA_BLACK);
                kprintf("\n  -- %d more processes --", n - rows_avail);
            }
        }

        /* ── Status bar ───────────────────────────────────────── */
        vga_set_cursor(0, VGA_HEIGHT - 1);
        vga_set_color(VGA_BLACK, VGA_WHITE);
        const char *sort_names[] = {
            [HTOP_SORT_PID]   = "PID",
            [HTOP_SORT_NAME]  = "NAME",
            [HTOP_SORT_CPU]   = "CPU",
            [HTOP_SORT_STATE] = "STATE",
            [HTOP_SORT_PRIO]  = "PRIO",
            [HTOP_SORT_NICE]  = "NICE",
        };
        kprintf(" Sort: %s  [q]uit [k]ill "
                "[p]id [n]ame [c]pu [s]tate [r]rio [i]nice",
                sort_names[sort_mode]);
        vga_set_color(VGA_WHITE, VGA_BLACK);

        /* ── Keyboard input ───────────────────────────────────── */
        if (keyboard_has_input()) {
            char ch = keyboard_getchar();
            switch (ch) {
            case 'q':
                running = 0;
                break;
            case 'k': {
                /* Prompt for PID to kill */
                vga_set_cursor(0, VGA_HEIGHT - 1);
                vga_set_color(VGA_BLACK, VGA_WHITE);
                kprintf("Signal to send (9=SIGKILL, 15=SIGTERM, 2=SIGINT): ");
                vga_set_color(VGA_WHITE, VGA_BLACK);
                char sigbuf[8];
                int siglen = 0;
                while (1) {
                    char c = keyboard_getchar();
                    if (c == '\r' || c == '\n') break;
                    if (c == 0x1B) { siglen = 0; break; } /* ESC */
                    if (c == 0x7F || c == '\b') { /* backspace */
                        if (siglen > 0) siglen--;
                        continue;
                    }
                    if (siglen < (int)sizeof(sigbuf) - 1)
                        sigbuf[siglen++] = c;
                }
                sigbuf[siglen] = '\0';
                int sig = (siglen > 0) ? parse_int(sigbuf) : 9;
                if (sig < 1 || sig > 31) sig = 9;

                /* Prompt for PID */
                vga_set_cursor(0, VGA_HEIGHT - 1);
                vga_set_color(VGA_BLACK, VGA_WHITE);
                kprintf("Enter PID to kill (signal %d): ", sig);
                vga_set_color(VGA_WHITE, VGA_BLACK);
                char pidbuf[16];
                int pidlen = 0;
                while (1) {
                    char c = keyboard_getchar();
                    if (c == '\r' || c == '\n') break;
                    if (c == 0x1B) { pidlen = 0; break; }
                    if (c == 0x7F || c == '\b') {
                        if (pidlen > 0) pidlen--;
                        continue;
                    }
                    if (pidlen < (int)sizeof(pidbuf) - 1)
                        pidbuf[pidlen++] = c;
                }
                pidbuf[pidlen] = '\0';
                uint32_t target_pid = (pidlen > 0) ? (uint32_t)parse_int(pidbuf) : 0;
                if (target_pid > 0) {
                    libc_kill(target_pid, sig);
                }
                break;
            }
            case 'p': sort_mode = HTOP_SORT_PID;   break;
            case 'n': sort_mode = HTOP_SORT_NAME;  break;
            case 'c': sort_mode = HTOP_SORT_CPU;   break;
            case 's': sort_mode = HTOP_SORT_STATE; break;
            case 'r': sort_mode = HTOP_SORT_PRIO;  break;
            case 'i': sort_mode = HTOP_SORT_NICE;  break;
            default:  break;
            }
        }

        /* ── Refresh rate ─────────────────────────────────────── */
        libc_sleep_ticks(TIMER_FREQ / 4); /* ~4 updates/sec */
    }

    vga_set_cursor(0, 0);
    kprintf("Exited htop.\n");
}

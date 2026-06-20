/*
 * cmd_nano.c — Nano-like text editor (Item U48)
 *
 * A simple full-screen text editor with familiar keybindings:
 *   Ctrl+X  Exit (prompt to save if modified)
 *   Ctrl+O  Write file (save)
 *   Ctrl+G  Help
 *   Arrow keys for navigation
 *   Backspace for deletion
 *   Auto-indent, line numbers
 *
 * Uses a dynamic line list for arbitrary-sized files.
 * Displays via VGA text mode (libc VGA wrappers).
 * Reads keyboard via libc_keyboard_getchar().
 * File I/O via libc_syscall().
 */

#include "shell_cmds.h"
#include "vga.h"
#include "libc.h"
#include "keyboard.h"  /* KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT */
#include "vga.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"
#include "types.h"
#include "syscall.h"

/* ── Constants ───────────────────────────────────────────────────────── */

#define NANO_TAB_SIZE           4
#define NANO_STATUS_LINES       2       /* status bar + shortcut bar */
#define NANO_MAX_LINE_LEN       4096

/* Ctrl-letter key codes */
#define KEY_CTRL_A      1
#define KEY_CTRL_B      2
#define KEY_CTRL_D      4
#define KEY_CTRL_E      5
#define KEY_CTRL_F      6
#define KEY_CTRL_G      7
#define KEY_CTRL_K      11
#define KEY_CTRL_N      14
#define KEY_CTRL_O      15
#define KEY_CTRL_R      18
#define KEY_CTRL_T      20
#define KEY_CTRL_U      21
#define KEY_CTRL_V      22
#define KEY_CTRL_W      23
#define KEY_CTRL_X      24
#define KEY_CTRL_Y      25
#define KEY_CTRL_Z      26
#define KEY_TAB         9
#define KEY_ENTER       13
#define KEY_BACKSPACE   127
#define KEY_ESC         27

/* VGA color helpers: fg | (bg << 4) */
#define COL_NORMAL       ((uint8_t)(VGA_WHITE | (VGA_BLUE << 4)))
#define COL_CURSOR       ((uint8_t)(VGA_BLACK | (VGA_CYAN << 4)))
#define COL_LINENUM      ((uint8_t)(VGA_WHITE | (VGA_BLUE << 4)))
#define COL_STATUSBAR    ((uint8_t)(VGA_WHITE | (VGA_BLACK << 4)))
#define COL_STATUSBAR_M  ((uint8_t)(VGA_WHITE | (VGA_RED << 4)))
#define COL_SHORTCUT     ((uint8_t)(VGA_BLACK | (VGA_WHITE << 4)))

/* ── Line structure — dynamic, doubly linked ─────────────────────────── */

struct nano_line {
    struct nano_line *next;
    struct nano_line *prev;
    char *text;
    int   len;
    int   cap;
};

/* ── Editor state ────────────────────────────────────────────────────── */

struct nano_state {
    struct nano_line *head;
    struct nano_line *tail;
    struct nano_line *cursor_line;
    int   cursor_x;
    int   cursor_y;
    int   scroll_x;
    int   scroll_y;
    int   modified;
    int   running;
    int   term_width;
    int   term_height;
    int   text_rows;
    char *filename;
    char  status_msg[80];
};

/* ── Line heap management ────────────────────────────────────────────── */

static struct nano_line *nano_line_alloc(int initial_cap)
{
    struct nano_line *l = (struct nano_line *)malloc(sizeof(struct nano_line));
    if (!l) return NULL;
    l->next = l->prev = NULL;
    if (initial_cap < 16) initial_cap = 16;
    if (initial_cap > NANO_MAX_LINE_LEN) initial_cap = NANO_MAX_LINE_LEN;
    l->text = (char *)malloc((size_t)initial_cap);
    if (!l->text) { free(l); return NULL; }
    l->text[0] = '\0';
    l->len = 0;
    l->cap = initial_cap;
    return l;
}

static void nano_line_free(struct nano_line *l)
{
    if (l) { free(l->text); free(l); }
}

static int nano_line_reserve(struct nano_line *l, int needed)
{
    if (!l || l->cap >= needed) return 0;
    int new_cap = l->cap * 2;
    while (new_cap < needed) new_cap *= 2;
    if (new_cap > NANO_MAX_LINE_LEN) new_cap = NANO_MAX_LINE_LEN;
    if (new_cap < needed) return -1;
    char *nt = (char *)realloc(l->text, (size_t)new_cap);
    if (!nt) return -1;
    l->text = nt;
    l->cap = new_cap;
    return 0;
}

static int nano_line_insert_char(struct nano_line *l, int pos, char ch)
{
    if (!l || pos < 0 || pos > l->len) return -1;
    if (nano_line_reserve(l, l->len + 2) < 0) return -1;
    memmove(l->text + pos + 1, l->text + pos, (size_t)(l->len - pos + 1));
    l->text[pos] = ch;
    l->len++;
    return 0;
}

static int nano_line_delete_char(struct nano_line *l, int pos)
{
    if (!l || pos < 0 || pos >= l->len) return -1;
    memmove(l->text + pos, l->text + pos + 1, (size_t)(l->len - pos));
    l->len--;
    return 0;
}

static struct nano_line *nano_insert_line_after(struct nano_state *ns,
                                                struct nano_line *after)
{
    struct nano_line *nl = nano_line_alloc(16);
    if (!nl) return NULL;
    if (!after) {
        nl->next = ns->head;
        if (ns->head) ns->head->prev = nl;
        ns->head = nl;
        if (!ns->tail) ns->tail = nl;
    } else {
        nl->next = after->next;
        nl->prev = after;
        if (after->next) after->next->prev = nl;
        after->next = nl;
        if (ns->tail == after) ns->tail = nl;
    }
    return nl;
}

static void nano_remove_line(struct nano_state *ns, struct nano_line *l)
{
    if (!l) return;
    if (l->prev) l->prev->next = l->next;
    else         ns->head = l->next;
    if (l->next) l->next->prev = l->prev;
    else         ns->tail = l->prev;
    nano_line_free(l);
}

/* ── File I/O ────────────────────────────────────────────────────────── */

static int nano_load_file(struct nano_state *ns, const char *path)
{
    int fd = (int)libc_syscall(SYS_OPEN, (uint64_t)(uintptr_t)path, 0, 0, 0, 0);
    if (fd < 0) return -1;

    char buf[4096];
    int total = 0, n;
    while ((n = (int)libc_syscall(SYS_READ, (uint64_t)fd,
               (uint64_t)(uintptr_t)(buf + total),
               (uint64_t)(sizeof(buf) - 1 - (size_t)total), 0, 0)) > 0) {
        total += n;
        if ((size_t)total >= sizeof(buf) - 1) break;
    }
    libc_syscall(SYS_CLOSE, (uint64_t)fd, 0, 0, 0, 0);
    buf[total] = '\0';

    struct nano_line *cur = ns->head;
    if (!cur) {
        cur = nano_line_alloc(128);
        if (!cur) return -1;
        ns->head = ns->tail = cur;
    }

    int i = 0;
    while (i < total) {
        int start = i;
        while (i < total && buf[i] != '\n') i++;
        int line_len = i - start;

        if (nano_line_reserve(cur, line_len + 1) < 0) return -1;
        memcpy(cur->text, buf + start, (size_t)line_len);
        cur->text[line_len] = '\0';
        cur->len = line_len;

        if (i < total && buf[i] == '\n') i++;
        if (i < total) {
            struct nano_line *nl = nano_insert_line_after(ns, cur);
            if (!nl) return -1;
            cur = nl;
        }
    }

    ns->cursor_line = ns->head;
    ns->cursor_x = ns->cursor_y = 0;
    ns->scroll_x = ns->scroll_y = 0;
    ns->modified = 0;
    return 0;
}

static int nano_save_file(struct nano_state *ns, const char *path)
{
    int fd = (int)libc_syscall(SYS_OPEN, (uint64_t)(uintptr_t)path,
                               0644, 0102, 0, 0);
    if (fd < 0)
        fd = (int)libc_syscall(SYS_OPEN, (uint64_t)(uintptr_t)path,
                               0644, 0x42, 0, 0);
    if (fd < 0) return -1;

    struct nano_line *l = ns->head;
    while (l) {
        if (l->len > 0)
            libc_syscall(SYS_WRITE, (uint64_t)fd,
                         (uint64_t)(uintptr_t)l->text, (uint64_t)l->len, 0, 0);
        libc_syscall(SYS_WRITE, (uint64_t)fd, (uint64_t)(uintptr_t)"\n", 1, 0, 0);
        l = l->next;
    }
    libc_syscall(SYS_CLOSE, (uint64_t)fd, 0, 0, 0, 0);
    ns->modified = 0;
    return 0;
}

/* ── Cursor & scroll helpers ────────────────────────────────────────── */

static struct nano_line *nano_line_at(struct nano_state *ns, int idx)
{
    struct nano_line *l = ns->head;
    for (int i = 0; l && i < idx; i++) l = l->next;
    return l;
}

static int nano_line_count(struct nano_state *ns)
{
    int n = 0;
    struct nano_line *l = ns->head;
    while (l) { n++; l = l->next; }
    return n > 0 ? n : 1;
}

static int nano_line_index(struct nano_state *ns, struct nano_line *l)
{
    int idx = 0;
    struct nano_line *p = ns->head;
    while (p && p != l) { idx++; p = p->next; }
    return idx;
}

static void nano_ensure_cursor_visible(struct nano_state *ns)
{
    int line_idx = nano_line_index(ns, ns->cursor_line);

    if (line_idx < ns->scroll_y)
        ns->scroll_y = line_idx;
    if (line_idx >= ns->scroll_y + ns->text_rows)
        ns->scroll_y = line_idx - ns->text_rows + 1;
    if (ns->cursor_y >= ns->text_rows)
        ns->cursor_y = ns->text_rows - 1;
    if (ns->cursor_y < 0)
        ns->cursor_y = 0;

    ns->cursor_y = line_idx - ns->scroll_y;

    if (ns->cursor_x < ns->scroll_x)
        ns->scroll_x = ns->cursor_x;
    if (ns->cursor_x >= ns->scroll_x + ns->term_width)
        ns->scroll_x = ns->cursor_x - ns->term_width + 1;
}

/* ── Editor actions ─────────────────────────────────────────────────── */

static void nano_insert_char(struct nano_state *ns, char ch)
{
    if (nano_line_insert_char(ns->cursor_line, ns->cursor_x, ch) == 0) {
        ns->cursor_x++;
        ns->modified = 1;
    }
}

static void nano_newline(struct nano_state *ns)
{
    struct nano_line *cl = ns->cursor_line;
    int rest = cl->len - ns->cursor_x;
    struct nano_line *nl = nano_insert_line_after(ns, cl);
    if (!nl) return;

    if (rest > 0) {
        if (nano_line_reserve(nl, rest + 1) < 0) { nano_remove_line(ns, nl); return; }
        memcpy(nl->text, cl->text + ns->cursor_x, (size_t)rest);
        nl->text[rest] = '\0';
        nl->len = rest;
        cl->text[ns->cursor_x] = '\0';
        cl->len = ns->cursor_x;
    }

    ns->cursor_line = nl;
    ns->cursor_x = 0;
    ns->modified = 1;
}

static void nano_backspace(struct nano_state *ns)
{
    if (ns->cursor_x > 0) {
        nano_line_delete_char(ns->cursor_line, ns->cursor_x - 1);
        ns->cursor_x--;
        ns->modified = 1;
    } else if (ns->cursor_line->prev) {
        struct nano_line *prev = ns->cursor_line->prev;
        int plen = prev->len;
        int clen = ns->cursor_line->len;
        if (nano_line_reserve(prev, plen + clen + 1) == 0) {
            memcpy(prev->text + plen, ns->cursor_line->text, (size_t)clen);
            prev->text[plen + clen] = '\0';
            prev->len = plen + clen;
        }
        ns->cursor_x = plen;
        ns->cursor_line = prev;
        nano_remove_line(ns, ns->cursor_line->next);
        ns->modified = 1;
    }
}

static void nano_cursor_left(struct nano_state *ns)
{
    if (ns->cursor_x > 0)
        ns->cursor_x--;
    else if (ns->cursor_line->prev) {
        ns->cursor_line = ns->cursor_line->prev;
        ns->cursor_x = ns->cursor_line->len;
    }
}

static void nano_cursor_right(struct nano_state *ns)
{
    if (ns->cursor_x < ns->cursor_line->len)
        ns->cursor_x++;
    else if (ns->cursor_line->next) {
        ns->cursor_line = ns->cursor_line->next;
        ns->cursor_x = 0;
    }
}

static void nano_cursor_up(struct nano_state *ns)
{
    if (ns->cursor_line->prev) {
        ns->cursor_line = ns->cursor_line->prev;
        if (ns->cursor_x > ns->cursor_line->len)
            ns->cursor_x = ns->cursor_line->len;
    }
}

static void nano_cursor_down(struct nano_state *ns)
{
    if (ns->cursor_line->next) {
        ns->cursor_line = ns->cursor_line->next;
        if (ns->cursor_x > ns->cursor_line->len)
            ns->cursor_x = ns->cursor_line->len;
    }
}

/* ── Screen output ──────────────────────────────────────────────────── */

static void nano_putchar_at(int col, int row, char ch, uint8_t color)
{
    vga_put_entry_at(ch, color, (uint16_t)row, (uint16_t)col);
}

static void nano_draw_line(struct nano_state *ns, int screen_row, int line_idx)
{
    struct nano_line *l = nano_line_at(ns, line_idx);
    int col = 0;

    /* Line number (4 chars right-aligned) */
    char num[8];
    int nlen = snprintf(num, sizeof(num), "%4d ", line_idx + 1);
    for (int i = 0; i < nlen && col < ns->term_width; i++, col++)
        nano_putchar_at(col, screen_row, num[i], COL_LINENUM);

    if (!l) {
        for (; col < ns->term_width; col++)
            nano_putchar_at(col, screen_row, ' ', COL_LINENUM);
        return;
    }

    /* Text with horizontal scrolling */
    int start_col = col;
    for (int i = ns->scroll_x; i < l->len && col < ns->term_width; i++, col++) {
        char ch = l->text[i];
        if (ch < 32) ch = '.';
        uint8_t c = COL_NORMAL;
        if (l == ns->cursor_line && i == ns->cursor_x)
            c = COL_CURSOR;
        nano_putchar_at(col, screen_row, ch, c);
    }

    /* Cursor at end of line */
    if (l == ns->cursor_line && ns->cursor_x >= l->len && col < ns->term_width) {
        int cc = start_col + (ns->cursor_x - ns->scroll_x);
        if (cc >= 0 && cc < ns->term_width)
            nano_putchar_at(cc, screen_row, ' ', COL_CURSOR);
    }

    for (; col < ns->term_width; col++)
        nano_putchar_at(col, screen_row, ' ', COL_NORMAL);
}

static void nano_draw_status_bar(struct nano_state *ns)
{
    char buf[80];
    const char *fn = ns->filename ? ns->filename : "[New File]";
    int line = nano_line_index(ns, ns->cursor_line) + 1;
    int tot  = nano_line_count(ns);
    int col  = ns->cursor_x + 1;

    int len = snprintf(buf, sizeof(buf), " %s%s  Line: %d/%d  Col: %d",
                       fn, ns->modified ? " [Modified]" : "", line, tot, col);
    if (len > (int)sizeof(buf) - 1) len = (int)sizeof(buf) - 1;

    int row = ns->term_height - 2;
    uint8_t color = ns->modified ? COL_STATUSBAR_M : COL_STATUSBAR;
    for (int i = 0; i < ns->term_width; i++)
        nano_putchar_at(i, row, (i < len) ? buf[i] : ' ', color);
}

static void nano_draw_shortcut_bar(struct nano_state *ns)
{
    char buf[80];
    int len = snprintf(buf, sizeof(buf),
                       "^G Help    ^O Write    ^X Exit    ^K Cut    ^U Paste");
    if (len > (int)sizeof(buf) - 1) len = (int)sizeof(buf) - 1;

    int row = ns->term_height - 1;
    for (int i = 0; i < ns->term_width; i++)
        nano_putchar_at(i, row, (i < len) ? buf[i] : ' ', COL_SHORTCUT);
}

static void nano_show_help(void)
{
    vga_clear();
    kprintf("=== Nano Help ===\n");
    kprintf("^X       Exit (prompts to save if modified)\n");
    kprintf("^O       Save file (Write Out)\n");
    kprintf("^G       Show this help\n");
    kprintf("^K       Cut current line\n");
    kprintf("^U       Paste cut line\n");
    kprintf("Arrow keys: navigation\n");
    kprintf("Backspace: delete characters\n");
    kprintf("Tab: insert spaces\n");
    kprintf("\nPress any key to return to editor...\n");
    keyboard_getchar();
}

static void nano_refresh_screen(struct nano_state *ns)
{
    nano_ensure_cursor_visible(ns);

    int line_idx = ns->scroll_y;
    for (int row = 0; row < ns->text_rows; row++, line_idx++)
        nano_draw_line(ns, row, line_idx);

    nano_draw_status_bar(ns);
    nano_draw_shortcut_bar(ns);

    vga_set_cursor((uint16_t)ns->cursor_y,
                   (uint16_t)(ns->cursor_x - ns->scroll_x));
}

/* ── Cut/Paste (single-line clipboard) ──────────────────────────────── */

static struct nano_line *nano_cut_line_buffer = NULL;

static void nano_cut_line(struct nano_state *ns)
{
    if (!ns->cursor_line) return;

    if (nano_cut_line_buffer) { nano_line_free(nano_cut_line_buffer); nano_cut_line_buffer = NULL; }

    nano_cut_line_buffer = nano_line_alloc(ns->cursor_line->len + 1);
    if (!nano_cut_line_buffer) return;
    memcpy(nano_cut_line_buffer->text, ns->cursor_line->text, (size_t)ns->cursor_line->len + 1);
    nano_cut_line_buffer->len = ns->cursor_line->len;

    struct nano_line *next = ns->cursor_line->next;
    struct nano_line *prev = ns->cursor_line->prev;
    nano_remove_line(ns, ns->cursor_line);

    if (next)       ns->cursor_line = next;
    else if (prev)  ns->cursor_line = prev;
    else            ns->cursor_line = nano_insert_line_after(ns, NULL);
    ns->cursor_x = 0;
    ns->modified = 1;
}

static void nano_paste_line(struct nano_state *ns)
{
    if (!nano_cut_line_buffer) return;
    struct nano_line *nl = nano_line_alloc(nano_cut_line_buffer->len + 1);
    if (!nl) return;
    memcpy(nl->text, nano_cut_line_buffer->text, (size_t)nano_cut_line_buffer->len + 1);
    nl->len = nano_cut_line_buffer->len;

    nl->next = ns->cursor_line->next;
    nl->prev = ns->cursor_line;
    if (ns->cursor_line->next) ns->cursor_line->next->prev = nl;
    ns->cursor_line->next = nl;
    if (ns->tail == ns->cursor_line) ns->tail = nl;

    ns->cursor_line = nl;
    ns->cursor_x = 0;
    ns->modified = 1;
}

/* ── Main editor loop ───────────────────────────────────────────────── */

static void nano_run(struct nano_state *ns)
{
    ns->running = 1;
    while (ns->running) {
        nano_refresh_screen(ns);

        unsigned char ch = (unsigned char)keyboard_getchar();

        switch (ch) {
        case (unsigned char)KEY_UP:    nano_cursor_up(ns);    break;
        case (unsigned char)KEY_DOWN:  nano_cursor_down(ns);  break;
        case (unsigned char)KEY_LEFT:  nano_cursor_left(ns);  break;
        case (unsigned char)KEY_RIGHT: nano_cursor_right(ns); break;

        case KEY_CTRL_X: {
            if (ns->modified) {
                vga_clear();
                kprintf("Save modified buffer? (y/N): ");
                unsigned char reply = (unsigned char)keyboard_getchar();
                if (reply == 'y' || reply == 'Y') {
                    if (ns->filename) {
                        nano_save_file(ns, ns->filename);
                    } else {
                        kprintf("\nFile name: ");
                        char fname[128];
                        int fi = 0;
                        while (fi < 127) {
                            unsigned char fc = (unsigned char)keyboard_getchar();
                            if (fc == KEY_ENTER) break;
                            if (fc == KEY_BACKSPACE && fi > 0) { fi--; continue; }
                            fname[fi++] = (char)fc;
                        }
                        fname[fi] = '\0';
                        if (fi > 0) {
                            free(ns->filename);
                            ns->filename = (char *)malloc((size_t)fi + 1);
                            if (ns->filename) {
                                memcpy(ns->filename, fname, (size_t)fi + 1);
                                nano_save_file(ns, ns->filename);
                            }
                        }
                    }
                }
            }
            ns->running = 0;
            break;
        }

        case KEY_CTRL_O: {
            if (ns->filename) {
                if (nano_save_file(ns, ns->filename) == 0)
                    snprintf(ns->status_msg, sizeof(ns->status_msg),
                             "Wrote %d lines", nano_line_count(ns));
                else
                    snprintf(ns->status_msg, sizeof(ns->status_msg),
                             "Error writing file!");
            } else {
                kprintf("\nFile name to write: ");
                char fname[128];
                int fi = 0;
                while (fi < 127) {
                    unsigned char fc = (unsigned char)keyboard_getchar();
                    if (fc == KEY_ENTER) break;
                    if (fc == KEY_BACKSPACE && fi > 0) { fi--; continue; }
                    fname[fi++] = (char)fc;
                }
                fname[fi] = '\0';
                if (fi > 0) {
                    free(ns->filename);
                    ns->filename = (char *)malloc((size_t)fi + 1);
                    if (ns->filename) {
                        memcpy(ns->filename, fname, (size_t)fi + 1);
                        if (nano_save_file(ns, ns->filename) == 0)
                            snprintf(ns->status_msg, sizeof(ns->status_msg),
                                     "Wrote %d lines", nano_line_count(ns));
                        else
                            snprintf(ns->status_msg, sizeof(ns->status_msg),
                                     "Error writing file!");
                    }
                }
            }
            break;
        }

        case KEY_CTRL_G:
            nano_show_help();
            break;

        case KEY_CTRL_K:
            nano_cut_line(ns);
            break;

        case KEY_CTRL_U:
            nano_paste_line(ns);
            break;

        case KEY_ENTER:
            nano_newline(ns);
            break;

        case KEY_BACKSPACE:
            nano_backspace(ns);
            break;

        case KEY_TAB: {
            int spaces = NANO_TAB_SIZE - (ns->cursor_x % NANO_TAB_SIZE);
            for (int i = 0; i < spaces; i++)
                nano_insert_char(ns, ' ');
            break;
        }

        default:
            if (ch >= 32)
                nano_insert_char(ns, (char)ch);
            break;
        }
    }
}

/* ── Cleanup ────────────────────────────────────────────────────────── */

static void nano_cleanup(struct nano_state *ns)
{
    struct nano_line *l = ns->head;
    while (l) {
        struct nano_line *next = l->next;
        nano_line_free(l);
        l = next;
    }
    ns->head = ns->tail = NULL;
    free(ns->filename);
    ns->filename = NULL;
}

/* ── Entry point ────────────────────────────────────────────────────── */

void cmd_nano(const char *args)
{
    struct nano_state ns;
    memset(&ns, 0, sizeof(ns));

    ns.term_width = 80;
    ns.term_height = 25;
    ns.text_rows = ns.term_height - NANO_STATUS_LINES;

    ns.head = nano_line_alloc(16);
    if (!ns.head) { kprintf("nano: out of memory\n"); return; }
    ns.tail = ns.head;
    ns.cursor_line = ns.head;

    /* Parse optional filename */
    if (args && *args) {
        while (*args == ' ') args++;
        if (*args) {
            const char *end = args;
            while (*end && *end != ' ') end++;
            int len = (int)(end - args);
            ns.filename = (char *)malloc((size_t)(len + 1));
            if (ns.filename) {
                memcpy(ns.filename, args, (size_t)len);
                ns.filename[len] = '\0';
                if (nano_load_file(&ns, ns.filename) < 0)
                    snprintf(ns.status_msg, sizeof(ns.status_msg),
                             "[New File] %s", ns.filename);
                else
                    snprintf(ns.status_msg, sizeof(ns.status_msg),
                             "Loaded %d lines", nano_line_count(&ns));
            }
        }
    }

    vga_clear();
    vga_set_cursor(0, 0);

    nano_run(&ns);
    nano_cleanup(&ns);

    if (nano_cut_line_buffer) {
        nano_line_free(nano_cut_line_buffer);
        nano_cut_line_buffer = NULL;
    }

    vga_clear();
    vga_set_cursor(0, 0);
}

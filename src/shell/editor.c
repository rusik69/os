/* editor.c — Dual-mode text editor: VGA console + ANSI/telnet */

#include "editor.h"
#include "vga.h"
#include "keyboard.h"
#include "fs.h"
#include "string.h"
#include "printf.h"
#include "serial.h"
#include "net.h"

#define ED_MAX_LINES  128
#define ED_LINE_LEN   80
#define ED_MAX_SIZE   (ED_MAX_LINES * ED_LINE_LEN)
#define ED_VIEW_ROWS  (VGA_HEIGHT - 2)   /* top status + bottom status bar */

/* ------------------------------------------------------------------ */
/*  Telnet input ring buffer                                           */
/* ------------------------------------------------------------------ */
#define ED_INPUT_BUF  256
static char ed_input_ring[ED_INPUT_BUF];
static int  ed_input_head = 0;
static int  ed_input_tail = 0;
static int  ed_telnet_mode = 0;

void editor_feed_char(char c) {
    int next = (ed_input_tail + 1) & (ED_INPUT_BUF - 1);
    if (next != ed_input_head) {
        ed_input_ring[ed_input_tail] = c;
        ed_input_tail = next;
    }
}

int editor_is_active(void) { return ed_telnet_mode; }

/* ------------------------------------------------------------------ */
/*  Editor state                                                       */
/* ------------------------------------------------------------------ */
static char ed_lines[ED_MAX_LINES][ED_LINE_LEN];
static int  ed_num_lines;
static int  ed_cx, ed_cy;     /* cursor x (col), y (line in buffer) */
static int  ed_scroll;        /* first visible line index */
static char ed_filename[64];
static int  ed_dirty;

/* ------------------------------------------------------------------ */
/*  Input abstraction                                                  */
/* ------------------------------------------------------------------ */

static char ed_readchar(void) {
    if (!ed_telnet_mode) return keyboard_getchar();
    while (ed_input_head == ed_input_tail) net_poll();
    char c = ed_input_ring[ed_input_head];
    ed_input_head = (ed_input_head + 1) & (ED_INPUT_BUF - 1);
    return c;
}

/* Returns KEY_UP/DOWN/LEFT/RIGHT constants or raw char value */
static int ed_getkey(void) {
    char c = ed_readchar();
    if (!ed_telnet_mode) return (unsigned char)c;
    /* Handle ANSI escape sequences for arrow keys */
    if (c == '\x1b') {
        char c2 = ed_readchar();
        if (c2 == '[') {
            char c3 = ed_readchar();
            switch (c3) {
            case 'A': return (unsigned char)KEY_UP;
            case 'B': return (unsigned char)KEY_DOWN;
            case 'C': return (unsigned char)KEY_RIGHT;
            case 'D': return (unsigned char)KEY_LEFT;
            case '3': ed_readchar(); return 4;   /* \x1b[3~ = Delete -> Ctrl-D */
            case 'H': return (unsigned char)KEY_LEFT;
            case 'F': return (unsigned char)KEY_RIGHT;
            default:  return '\x1b';
            }
        }
        return '\x1b';
    }
    return (unsigned char)c;
}

/* ------------------------------------------------------------------ */
/*  VGA rendering                                                      */
/* ------------------------------------------------------------------ */

static void vga_ed_clear(void) {
    for (int r = 0; r < VGA_HEIGHT; r++)
        for (int c = 0; c < VGA_WIDTH; c++)
            vga_put_entry_at(' ', VGA_LIGHT_GREY | (VGA_BLACK << 4), r, c);
}

static void vga_ed_status_top(void) {
    uint8_t color = VGA_BLACK | (VGA_LIGHT_GREY << 4);
    for (int c = 0; c < VGA_WIDTH; c++)
        vga_put_entry_at(' ', color, 0, c);
    const char *title = "EDIT: ";
    int col = 0;
    while (*title && col < VGA_WIDTH) vga_put_entry_at(*title++, color, 0, col++);
    const char *fn = ed_filename;
    while (*fn && col < VGA_WIDTH) vga_put_entry_at(*fn++, color, 0, col++);
    if (ed_dirty && col < VGA_WIDTH - 2) {
        vga_put_entry_at(' ', color, 0, col++);
        vga_put_entry_at('*', color, 0, col++);
    }
}

static void vga_ed_status_bottom(void) {
    uint8_t color = VGA_BLACK | (VGA_LIGHT_GREY << 4);
    int row = VGA_HEIGHT - 1;
    for (int c = 0; c < VGA_WIDTH; c++)
        vga_put_entry_at(' ', color, row, c);
    const char *help = "Ctrl-S:Save  Ctrl-Q:Quit  Arrows:Move  Del:Del";
    int col = 0;
    while (*help && col < VGA_WIDTH) vga_put_entry_at(*help++, color, row, col++);

    char pos[20]; int n = 0;
    char tmp[10]; int t = 0;
    int v = ed_cy + 1;
    if (!v) { tmp[t++] = '0'; } else { while (v) { tmp[t++] = '0' + v%10; v /= 10; } }
    for (int i = t-1; i >= 0; i--) pos[n++] = tmp[i];
    pos[n++] = ':'; t = 0;
    v = ed_cx + 1;
    if (!v) { tmp[t++] = '0'; } else { while (v) { tmp[t++] = '0' + v%10; v /= 10; } }
    for (int i = t-1; i >= 0; i--) pos[n++] = tmp[i];
    pos[n] = '\0';
    col = VGA_WIDTH - n - 1;
    if (col < 0) col = 0;
    for (int i = 0; i < n; i++)
        vga_put_entry_at(pos[i], color, row, col + i);
}

static void vga_ed_lines(void) {
    uint8_t color = VGA_LIGHT_GREY | (VGA_BLACK << 4);
    for (int r = 0; r < ED_VIEW_ROWS; r++) {
        int line = ed_scroll + r;
        int scr_row = r + 1;
        if (line < ed_num_lines) {
            int len = strlen(ed_lines[line]);
            int c;
            for (c = 0; c < len && c < VGA_WIDTH; c++)
                vga_put_entry_at(ed_lines[line][c], color, scr_row, c);
            for (; c < VGA_WIDTH; c++)
                vga_put_entry_at(' ', color, scr_row, c);
        } else {
            vga_put_entry_at('~', VGA_DARK_GREY | (VGA_BLACK << 4), scr_row, 0);
            for (int c = 1; c < VGA_WIDTH; c++)
                vga_put_entry_at(' ', color, scr_row, c);
        }
    }
}

static void vga_ed_refresh(void) {
    if (ed_cy < ed_scroll) ed_scroll = ed_cy;
    if (ed_cy >= ed_scroll + ED_VIEW_ROWS) ed_scroll = ed_cy - ED_VIEW_ROWS + 1;
    vga_ed_status_top();
    vga_ed_lines();
    vga_ed_status_bottom();
    int scr_row = ed_cy - ed_scroll + 1;
    int scr_col = ed_cx < VGA_WIDTH ? ed_cx : VGA_WIDTH - 1;
    vga_set_cursor(scr_row, scr_col);
}

/* ------------------------------------------------------------------ */
/*  ANSI rendering (telnet mode)                                       */
/* ------------------------------------------------------------------ */

static void ansi_goto(int row, int col) {
    kprintf("\x1b[%d;%dH", row + 1, col + 1);
}

static void ansi_ed_refresh(void) {
    if (ed_cy < ed_scroll) ed_scroll = ed_cy;
    if (ed_cy >= ed_scroll + ED_VIEW_ROWS) ed_scroll = ed_cy - ED_VIEW_ROWS + 1;

    /* Top status bar */
    ansi_goto(0, 0);
    kprintf("\x1b[7m");
    kprintf("EDIT: %s%s", ed_filename, ed_dirty ? " *" : "  ");
    int used = 6 + (int)strlen(ed_filename) + 2;
    for (int i = used; i < VGA_WIDTH; i++) kprintf(" ");
    kprintf("\x1b[0m");

    /* Content lines */
    for (int r = 0; r < ED_VIEW_ROWS; r++) {
        ansi_goto(r + 1, 0);
        int line = ed_scroll + r;
        if (line < ed_num_lines) {
            kprintf("%s\x1b[K", ed_lines[line]);
        } else {
            kprintf("\x1b[34m~\x1b[0m\x1b[K");
        }
    }

    /* Bottom status bar */
    ansi_goto(VGA_HEIGHT - 1, 0);
    kprintf("\x1b[7m");
    const char *help = "Ctrl-S:Save  Ctrl-Q:Quit  Arrows:Move  Del:Del";
    kprintf("%s", help);
    int help_len = (int)strlen(help);

    /* Line:Col on right */
    char pos[16]; int pn = 0;
    char tmp[8]; int ti = 0;
    int v = ed_cy + 1;
    if (!v) { tmp[ti++] = '0'; } else { while (v) { tmp[ti++] = '0' + v%10; v /= 10; } }
    for (int i = ti-1; i >= 0; i--) pos[pn++] = tmp[i];
    pos[pn++] = ':'; ti = 0;
    v = ed_cx + 1;
    if (!v) { tmp[ti++] = '0'; } else { while (v) { tmp[ti++] = '0' + v%10; v /= 10; } }
    for (int i = ti-1; i >= 0; i--) pos[pn++] = tmp[i];
    pos[pn] = '\0';
    int pad = VGA_WIDTH - help_len - pn - 1;
    if (pad > 0) for (int i = 0; i < pad; i++) kprintf(" ");
    kprintf(" %s\x1b[0m", pos);

    /* Position cursor */
    int scr_col = ed_cx < VGA_WIDTH ? ed_cx : VGA_WIDTH - 1;
    ansi_goto(ed_cy - ed_scroll + 1, scr_col);

    kprintf_flush();
}

static void ed_refresh(void) {
    if (ed_telnet_mode) ansi_ed_refresh();
    else                vga_ed_refresh();
}

/* ------------------------------------------------------------------ */
/*  Buffer operations                                                  */
/* ------------------------------------------------------------------ */

static int ed_line_len(int line) {
    if (line < 0 || line >= ed_num_lines) return 0;
    return (int)strlen(ed_lines[line]);
}

static void ed_insert_char(char c) {
    int len = ed_line_len(ed_cy);
    if (len >= ED_LINE_LEN - 1) return;
    for (int i = len; i > ed_cx; i--)
        ed_lines[ed_cy][i] = ed_lines[ed_cy][i - 1];
    ed_lines[ed_cy][ed_cx] = c;
    ed_lines[ed_cy][len + 1] = '\0';
    ed_cx++;
    ed_dirty = 1;
}

static void ed_insert_newline(void) {
    if (ed_num_lines >= ED_MAX_LINES) return;
    for (int i = ed_num_lines; i > ed_cy + 1; i--)
        memcpy(ed_lines[i], ed_lines[i - 1], ED_LINE_LEN);
    ed_num_lines++;
    int len = ed_line_len(ed_cy);
    int tail = len - ed_cx;
    memcpy(ed_lines[ed_cy + 1], ed_lines[ed_cy] + ed_cx, tail);
    ed_lines[ed_cy + 1][tail] = '\0';
    ed_lines[ed_cy][ed_cx] = '\0';
    ed_cy++;
    ed_cx = 0;
    ed_dirty = 1;
}

static void ed_backspace(void) {
    if (ed_cx > 0) {
        int len = ed_line_len(ed_cy);
        for (int i = ed_cx - 1; i < len; i++)
            ed_lines[ed_cy][i] = ed_lines[ed_cy][i + 1];
        ed_cx--;
        ed_dirty = 1;
    } else if (ed_cy > 0) {
        int prev_len = ed_line_len(ed_cy - 1);
        int cur_len  = ed_line_len(ed_cy);
        if (prev_len + cur_len < ED_LINE_LEN) {
            memcpy(ed_lines[ed_cy - 1] + prev_len, ed_lines[ed_cy], cur_len + 1);
            for (int i = ed_cy; i < ed_num_lines - 1; i++)
                memcpy(ed_lines[i], ed_lines[i + 1], ED_LINE_LEN);
            ed_num_lines--;
            memset(ed_lines[ed_num_lines], 0, ED_LINE_LEN);
            ed_cy--;
            ed_cx = prev_len;
            ed_dirty = 1;
        }
    }
}

static void ed_delete_char(void) {
    int len = ed_line_len(ed_cy);
    if (ed_cx < len) {
        for (int i = ed_cx; i < len; i++)
            ed_lines[ed_cy][i] = ed_lines[ed_cy][i + 1];
        ed_dirty = 1;
    } else if (ed_cy < ed_num_lines - 1) {
        int next_len = ed_line_len(ed_cy + 1);
        if (len + next_len < ED_LINE_LEN) {
            memcpy(ed_lines[ed_cy] + len, ed_lines[ed_cy + 1], next_len + 1);
            for (int i = ed_cy + 1; i < ed_num_lines - 1; i++)
                memcpy(ed_lines[i], ed_lines[i + 1], ED_LINE_LEN);
            ed_num_lines--;
            memset(ed_lines[ed_num_lines], 0, ED_LINE_LEN);
            ed_dirty = 1;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Save / Load                                                        */
/* ------------------------------------------------------------------ */

static void ed_save(void) {
    char buf[ED_MAX_SIZE];
    int pos = 0;
    for (int i = 0; i < ed_num_lines; i++) {
        int len = ed_line_len(i);
        if (pos + len + 1 >= ED_MAX_SIZE) break;
        memcpy(buf + pos, ed_lines[i], len);
        pos += len;
        if (i < ed_num_lines - 1) buf[pos++] = '\n';
    }
    char path[66];
    if (ed_filename[0] != '/') { path[0] = '/'; strcpy(path + 1, ed_filename); }
    else strcpy(path, ed_filename);

    if (fs_write_file(path, buf, pos) < 0) {
        if (ed_telnet_mode) {
            kprintf("\x1b[%d;1H\x1b[41m SAVE FAILED \x1b[0m", VGA_HEIGHT);
            kprintf_flush();
        } else {
            uint8_t color = VGA_WHITE | (VGA_RED << 4);
            const char *msg = " SAVE FAILED ";
            for (int i = 0; msg[i] && i < VGA_WIDTH; i++)
                vga_put_entry_at(msg[i], color, VGA_HEIGHT - 1, i);
        }
    } else {
        ed_dirty = 0;
    }
}

static void ed_load(void) {
    char path[66];
    if (ed_filename[0] != '/') { path[0] = '/'; strcpy(path + 1, ed_filename); }
    else strcpy(path, ed_filename);

    char buf[ED_MAX_SIZE];
    uint32_t size = 0;

    memset(ed_lines, 0, sizeof(ed_lines));
    ed_num_lines = 1;
    ed_cx = 0; ed_cy = 0; ed_scroll = 0; ed_dirty = 0;

    if (fs_read_file(path, buf, ED_MAX_SIZE - 1, &size) < 0) return; /* new file */
    buf[size] = '\0';

    ed_num_lines = 0;
    int col = 0;
    for (uint32_t i = 0; i < size && ed_num_lines < ED_MAX_LINES; i++) {
        if (buf[i] == '\n') {
            ed_lines[ed_num_lines][col] = '\0';
            ed_num_lines++;
            col = 0;
        } else if (col < ED_LINE_LEN - 1) {
            ed_lines[ed_num_lines][col++] = buf[i];
        }
    }
    if (col > 0 || ed_num_lines == 0) {
        ed_lines[ed_num_lines][col] = '\0';
        ed_num_lines++;
    }
}

/* ------------------------------------------------------------------ */
/*  Main entry point                                                   */
/* ------------------------------------------------------------------ */

void editor_open(const char *filename) {
    if (!filename || !filename[0]) {
        kprintf("Usage: edit <filename>\n");
        return;
    }

    strncpy(ed_filename, filename, sizeof(ed_filename) - 1);
    ed_filename[sizeof(ed_filename) - 1] = '\0';

    /* Telnet mode if kprintf is currently hooked to a network session */
    void (*hook)(char,void*) = 0; void *hctx = 0;
    kprintf_get_hook(&hook, &hctx);
    ed_telnet_mode = (hook != 0);

    /* Reset input ring */
    ed_input_head = ed_input_tail = 0;

    ed_load();

    if (ed_telnet_mode) {
        kprintf("\x1b[2J");   /* clear screen */
        kprintf_flush();
    } else {
        vga_ed_clear();
    }

    ed_refresh();

    int running = 1;
    while (running) {
        int k = ed_getkey();

        switch ((unsigned char)k) {
        case (unsigned char)KEY_UP:
            if (ed_cy > 0) {
                ed_cy--;
                if (ed_cx > ed_line_len(ed_cy)) ed_cx = ed_line_len(ed_cy);
            }
            break;
        case (unsigned char)KEY_DOWN:
            if (ed_cy < ed_num_lines - 1) {
                ed_cy++;
                if (ed_cx > ed_line_len(ed_cy)) ed_cx = ed_line_len(ed_cy);
            }
            break;
        case (unsigned char)KEY_LEFT:
            if (ed_cx > 0) ed_cx--;
            else if (ed_cy > 0) { ed_cy--; ed_cx = ed_line_len(ed_cy); }
            break;
        case (unsigned char)KEY_RIGHT:
            if (ed_cx < ed_line_len(ed_cy)) ed_cx++;
            else if (ed_cy < ed_num_lines - 1) { ed_cy++; ed_cx = 0; }
            break;
        case 19: /* Ctrl-S: save */
            ed_save();
            break;
        case 17: /* Ctrl-Q: quit */
            running = 0;
            break;
        case 4:  /* Ctrl-D: delete char at cursor */
            ed_delete_char();
            break;
        case '\n':
        case '\r':
            ed_insert_newline();
            break;
        case '\b':
        case 127:
            ed_backspace();
            break;
        case '\t':
            for (int i = 0; i < 4; i++) ed_insert_char(' ');
            break;
        default:
            if (k >= 32 && k < 127) ed_insert_char((char)k);
            break;
        }

        ed_refresh();
    }

    int was_telnet = ed_telnet_mode;
    ed_telnet_mode = 0;

    if (was_telnet) {
        kprintf("\x1b[2J\x1b[H");
        kprintf_flush();
    } else {
        vga_clear();
        vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        vga_set_cursor(0, 0);
    }
}

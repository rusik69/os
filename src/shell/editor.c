#include "editor.h"
#include "vga.h"
#include "keyboard.h"
#include "fs.h"
#include "string.h"
#include "printf.h"
#include "serial.h"

#define ED_MAX_LINES  128
#define ED_LINE_LEN   80
#define ED_MAX_SIZE   (ED_MAX_LINES * ED_LINE_LEN)
#define ED_VIEW_ROWS  (VGA_HEIGHT - 2)  /* top status + bottom status */

static char ed_lines[ED_MAX_LINES][ED_LINE_LEN];
static int ed_num_lines;
static int ed_cx, ed_cy;       /* cursor x (col), y (line in buffer) */
static int ed_scroll;          /* first visible line */
static char ed_filename[64];
static int ed_dirty;

static void ed_clear_screen(void) {
    for (int r = 0; r < VGA_HEIGHT; r++)
        for (int c = 0; c < VGA_WIDTH; c++)
            vga_put_entry_at(' ', VGA_LIGHT_GREY | (VGA_BLACK << 4), r, c);
}

static void ed_draw_status_top(void) {
    uint8_t color = VGA_BLACK | (VGA_LIGHT_GREY << 4);
    for (int c = 0; c < VGA_WIDTH; c++)
        vga_put_entry_at(' ', color, 0, c);

    const char *title = "EDIT: ";
    int col = 0;
    while (*title && col < VGA_WIDTH) {
        vga_put_entry_at(*title++, color, 0, col++);
    }
    const char *fn = ed_filename;
    while (*fn && col < VGA_WIDTH) {
        vga_put_entry_at(*fn++, color, 0, col++);
    }
    if (ed_dirty && col < VGA_WIDTH - 2) {
        vga_put_entry_at(' ', color, 0, col++);
        vga_put_entry_at('*', color, 0, col++);
    }
}

static void ed_draw_status_bottom(void) {
    uint8_t color = VGA_BLACK | (VGA_LIGHT_GREY << 4);
    int row = VGA_HEIGHT - 1;
    for (int c = 0; c < VGA_WIDTH; c++)
        vga_put_entry_at(' ', color, row, c);

    const char *help = "Ctrl-S:Save  Ctrl-Q:Quit  Ctrl-G:Goto  Arrows:Move";
    int col = 0;
    while (*help && col < VGA_WIDTH) {
        vga_put_entry_at(*help++, color, row, col++);
    }

    /* Line:Col indicator on the right */
    char pos[20];
    int n = 0;
    /* Simple int-to-str for line:col */
    int line = ed_cy + 1;
    int column = ed_cx + 1;
    char tmp[10];
    int t = 0;
    if (line == 0) tmp[t++] = '0';
    else { int v = line; while (v) { tmp[t++] = '0' + v % 10; v /= 10; } }
    for (int i = t - 1; i >= 0; i--) pos[n++] = tmp[i];
    pos[n++] = ':';
    t = 0;
    if (column == 0) tmp[t++] = '0';
    else { int v = column; while (v) { tmp[t++] = '0' + v % 10; v /= 10; } }
    for (int i = t - 1; i >= 0; i--) pos[n++] = tmp[i];
    pos[n] = 0;

    col = VGA_WIDTH - n - 1;
    if (col < 0) col = 0;
    for (int i = 0; i < n; i++)
        vga_put_entry_at(pos[i], color, row, col + i);
}

static void ed_draw_lines(void) {
    uint8_t color = VGA_LIGHT_GREY | (VGA_BLACK << 4);
    for (int r = 0; r < ED_VIEW_ROWS; r++) {
        int line = ed_scroll + r;
        int scr_row = r + 1; /* skip top status bar */
        if (line < ed_num_lines) {
            int len = strlen(ed_lines[line]);
            int c;
            for (c = 0; c < len && c < VGA_WIDTH; c++)
                vga_put_entry_at(ed_lines[line][c], color, scr_row, c);
            for (; c < VGA_WIDTH; c++)
                vga_put_entry_at(' ', color, scr_row, c);
        } else {
            /* Tilde for empty lines */
            vga_put_entry_at('~', VGA_DARK_GREY | (VGA_BLACK << 4), scr_row, 0);
            for (int c = 1; c < VGA_WIDTH; c++)
                vga_put_entry_at(' ', color, scr_row, c);
        }
    }
}

static void ed_refresh(void) {
    /* Adjust scroll to keep cursor visible */
    if (ed_cy < ed_scroll) ed_scroll = ed_cy;
    if (ed_cy >= ed_scroll + ED_VIEW_ROWS) ed_scroll = ed_cy - ED_VIEW_ROWS + 1;

    ed_draw_status_top();
    ed_draw_lines();
    ed_draw_status_bottom();

    /* Position hardware cursor */
    int scr_row = ed_cy - ed_scroll + 1;
    int scr_col = ed_cx;
    if (scr_col >= VGA_WIDTH) scr_col = VGA_WIDTH - 1;
    vga_set_cursor(scr_row, scr_col);
}

static int ed_line_len(int line) {
    if (line < 0 || line >= ed_num_lines) return 0;
    return strlen(ed_lines[line]);
}

static void ed_insert_char(char c) {
    int len = ed_line_len(ed_cy);
    if (len >= ED_LINE_LEN - 1) return;

    /* Shift right */
    for (int i = len; i > ed_cx; i--)
        ed_lines[ed_cy][i] = ed_lines[ed_cy][i - 1];
    ed_lines[ed_cy][ed_cx] = c;
    ed_lines[ed_cy][len + 1] = '\0';
    ed_cx++;
    ed_dirty = 1;
}

static void ed_insert_newline(void) {
    if (ed_num_lines >= ED_MAX_LINES) return;

    /* Move lines down */
    for (int i = ed_num_lines; i > ed_cy + 1; i--)
        memcpy(ed_lines[i], ed_lines[i - 1], ED_LINE_LEN);

    ed_num_lines++;

    /* Split current line at cursor */
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
        /* Join with previous line */
        int prev_len = ed_line_len(ed_cy - 1);
        int cur_len = ed_line_len(ed_cy);
        if (prev_len + cur_len < ED_LINE_LEN) {
            memcpy(ed_lines[ed_cy - 1] + prev_len, ed_lines[ed_cy], cur_len + 1);
            /* Shift lines up */
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
        /* Join with next line */
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

static void ed_save(void) {
    /* Flatten lines into buffer */
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
    if (ed_filename[0] != '/') {
        path[0] = '/';
        strcpy(path + 1, ed_filename);
    } else {
        strcpy(path, ed_filename);
    }

    if (fs_write_file(path, buf, pos) < 0) {
        /* Show error briefly in status */
        uint8_t color = VGA_WHITE | (VGA_RED << 4);
        const char *msg = " SAVE FAILED ";
        int row = VGA_HEIGHT - 1;
        for (int i = 0; msg[i] && i < VGA_WIDTH; i++)
            vga_put_entry_at(msg[i], color, row, i);
    } else {
        ed_dirty = 0;
    }
}

static void ed_load(void) {
    char path[66];
    if (ed_filename[0] != '/') {
        path[0] = '/';
        strcpy(path + 1, ed_filename);
    } else {
        strcpy(path, ed_filename);
    }

    char buf[ED_MAX_SIZE];
    uint32_t size = 0;

    memset(ed_lines, 0, sizeof(ed_lines));
    ed_num_lines = 1; /* at least one line */
    ed_cx = 0;
    ed_cy = 0;
    ed_scroll = 0;
    ed_dirty = 0;

    if (fs_read_file(path, buf, ED_MAX_SIZE - 1, &size) < 0) {
        /* New file */
        return;
    }
    buf[size] = '\0';

    /* Parse lines */
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
    /* Last line (no trailing \n) */
    if (col > 0 || ed_num_lines == 0) {
        ed_lines[ed_num_lines][col] = '\0';
        ed_num_lines++;
    }
}

void editor_open(const char *filename) {
    if (!filename || !filename[0]) {
        kprintf("Usage: edit <filename>\n");
        return;
    }

    strncpy(ed_filename, filename, sizeof(ed_filename) - 1);
    ed_filename[sizeof(ed_filename) - 1] = '\0';

    ed_load();
    ed_clear_screen();
    ed_refresh();

    int running = 1;
    while (running) {
        char c = keyboard_getchar();

        switch (c) {
        case KEY_UP:
            if (ed_cy > 0) {
                ed_cy--;
                if (ed_cx > ed_line_len(ed_cy)) ed_cx = ed_line_len(ed_cy);
            }
            break;
        case KEY_DOWN:
            if (ed_cy < ed_num_lines - 1) {
                ed_cy++;
                if (ed_cx > ed_line_len(ed_cy)) ed_cx = ed_line_len(ed_cy);
            }
            break;
        case KEY_LEFT:
            if (ed_cx > 0) ed_cx--;
            else if (ed_cy > 0) { ed_cy--; ed_cx = ed_line_len(ed_cy); }
            break;
        case KEY_RIGHT:
            if (ed_cx < ed_line_len(ed_cy)) ed_cx++;
            else if (ed_cy < ed_num_lines - 1) { ed_cy++; ed_cx = 0; }
            break;
        case 19: /* Ctrl-S: save */
            ed_save();
            break;
        case 17: /* Ctrl-Q: quit */
            running = 0;
            break;
        case 4: /* Ctrl-D: delete char at cursor */
            ed_delete_char();
            break;
        case '\n':
            ed_insert_newline();
            break;
        case '\b':
        case 127: /* DEL key */
            ed_backspace();
            break;
        case '\t':
            /* Insert spaces for tab */
            for (int i = 0; i < 4; i++) ed_insert_char(' ');
            break;
        default:
            if (c >= 32 && c < 127) {
                ed_insert_char(c);
            }
            break;
        }

        ed_refresh();
    }

    /* Restore normal screen */
    vga_clear();
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    vga_set_cursor(0, 0);
}

/* editor.c — Dual-mode text editor: VGA console + ANSI/telnet
 *
 * Features:
 *   - Edit up to 1024 lines per buffer (dynamically allocated line buffer)
 *   - Multi-buffer: up to 4 simultaneously open buffers
 *   - Clipboard: cut (Ctrl-X), copy (Ctrl-C), paste (Ctrl-V)
 *   - Yank whole line (Ctrl-Y), delete line (Alt-D)
 *   - Buffer switching: Ctrl-N (next), Ctrl-P (prev)
 *   - :e filename, :bn, :bp in command mode
 *   - VGA console and ANSI/telnet output
 */

#include "editor.h"
#include "vga.h"
#include "keyboard.h"
#include "fs.h"
#include "string.h"
#include "printf.h"
#include "serial.h"
#include "net.h"

/* ── Limits ──────────────────────────────────────────────────────── */
#define ED_MAX_BUFS     4          /* number of simultaneously open buffers */
#define ED_MAX_LINES    1024       /* lines per buffer (dynamic array) */
#define ED_LINE_LEN     256        /* max characters per line */
#define ED_LINE_ALLOC   256        /* allocation quantum for line growth */
#define ED_VIEW_ROWS    (VGA_HEIGHT - 2)  /* top status + bottom status bar */
#define ED_CLIP_LEN     256        /* clipboard maximum length */

/* ── Telnet input ring buffer ────────────────────────────────────── */
#define ED_INPUT_BUF  512
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

/* ── Per-buffer state ────────────────────────────────────────────── */
struct ed_buffer {
    char  lines[ED_MAX_LINES][ED_LINE_LEN];
    int   num_lines;
    int   cx, cy;            /* cursor x (col), y (line in buffer) */
    int   scroll;            /* first visible line index */
    char  filename[64];
    int   dirty;
    int   in_use;
};

static struct ed_buffer ed_buffers[ED_MAX_BUFS];
static int              ed_cur_buf = 0;  /* index of active buffer */

/* ── Clipboard ───────────────────────────────────────────────────── */
static char ed_clipboard[ED_CLIP_LEN];
static int  ed_clip_len = 0;

/* ── Helpers ─────────────────────────────────────────────────────── */
static struct ed_buffer *cur_buf(void) {
    return &ed_buffers[ed_cur_buf];
}

#define BUF() cur_buf()

/* ── Input abstraction ───────────────────────────────────────────── */
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
    /* Handle ANSI escape sequences for arrow keys and function keys */
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
            case '5': return (unsigned char)KEY_PAGEUP;   /* \x1b[5~ = Page Up */
            case '6': return (unsigned char)KEY_PAGEDOWN; /* \x1b[6~ = Page Dn */
            default:  return '\x1b';
            }
        }
        if (c2 == 'O') {
            char c3 = ed_readchar();
            /* \x1bOP = F1, \x1bOQ = F2, \x1bOR = F3, \x1bOS = F4 */
            if (c3 == 'P') return 128; /* F1 */
            if (c3 == 'Q') return 129; /* F2 */
            if (c3 == 'R') return 130; /* F3 */
            if (c3 == 'S') return 131; /* F4 */
        }
        return '\x1b';
    }
    return (unsigned char)c;
}

/* ── Line length helper ──────────────────────────────────────────── */
static int ed_line_len(int line) {
    struct ed_buffer *b = BUF();
    if (line < 0 || line >= b->num_lines) return 0;
    return (int)strlen(b->lines[line]);
}

/* ── VGA rendering ───────────────────────────────────────────────── */
static void vga_ed_clear(void) {
    for (int r = 0; r < VGA_HEIGHT; r++)
        for (int c = 0; c < VGA_WIDTH; c++)
            vga_put_entry_at(' ', VGA_LIGHT_GREY | (VGA_BLACK << 4), r, c);
}

static void vga_ed_status_top(void) {
    struct ed_buffer *b = BUF();
    uint8_t color = VGA_BLACK | (VGA_LIGHT_GREY << 4);
    for (int c = 0; c < VGA_WIDTH; c++)
        vga_put_entry_at(' ', color, 0, c);
    char title[VGA_WIDTH + 1];
    int pos = 0;
    pos += snprintf(title + pos, sizeof(title) - (size_t)pos,
                    "EDIT: %s%s  [Buf %d/%d]",
                    b->filename, b->dirty ? " *" : "  ",
                    ed_cur_buf + 1, ED_MAX_BUFS);
    int col = 0;
    while (title[col] && col < VGA_WIDTH) {
        vga_put_entry_at(title[col], color, 0, col);
        col++;
    }
}

static void vga_ed_status_bottom(void) {
    struct ed_buffer *b = BUF();
    uint8_t color = VGA_BLACK | (VGA_LIGHT_GREY << 4);
    int row = VGA_HEIGHT - 1;
    for (int c = 0; c < VGA_WIDTH; c++)
        vga_put_entry_at(' ', color, row, c);
    const char *help = "^S:Save ^Q:Quit ^X:Cut ^C:Copy ^V:Paste ^N:NextBuf ^P:PrevBuf";
    int col = 0;
    while (*help && col < VGA_WIDTH)
        vga_put_entry_at(*help++, color, row, col++);

    char pos[20]; int n = 0;
    char tmp[10]; int t = 0;
    int v = b->cy + 1;
    if (!v) { tmp[t++] = '0'; } else { while (v) { tmp[t++] = '0' + v%10; v /= 10; } }
    for (int i = t-1; i >= 0; i--) pos[n++] = tmp[i];
    pos[n++] = ':'; t = 0;
    v = b->cx + 1;
    if (!v) { tmp[t++] = '0'; } else { while (v) { tmp[t++] = '0' + v%10; v /= 10; } }
    for (int i = t-1; i >= 0; i--) pos[n++] = tmp[i];
    pos[n] = '\0';
    col = VGA_WIDTH - n - 1;
    if (col < 0) col = 0;
    for (int i = 0; i < n; i++)
        vga_put_entry_at(pos[i], color, row, col + i);
}

static void vga_ed_lines(void) {
    struct ed_buffer *b = BUF();
    uint8_t color = VGA_LIGHT_GREY | (VGA_BLACK << 4);
    for (int r = 0; r < ED_VIEW_ROWS; r++) {
        int line = b->scroll + r;
        int scr_row = r + 1;
        if (line < b->num_lines) {
            int len = (int)strlen(b->lines[line]);
            int c;
            for (c = 0; c < len && c < VGA_WIDTH; c++)
                vga_put_entry_at(b->lines[line][c], color, scr_row, c);
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
    struct ed_buffer *b = BUF();
    if (b->cy < b->scroll) b->scroll = b->cy;
    if (b->cy >= b->scroll + ED_VIEW_ROWS) b->scroll = b->cy - ED_VIEW_ROWS + 1;
    vga_ed_status_top();
    vga_ed_lines();
    vga_ed_status_bottom();
    int scr_row = b->cy - b->scroll + 1;
    int scr_col = b->cx < VGA_WIDTH ? b->cx : VGA_WIDTH - 1;
    vga_set_cursor(scr_row, scr_col);
}

/* ── ANSI rendering (telnet mode) ────────────────────────────────── */
static void ansi_goto(int row, int col) {
    kprintf("\x1b[%d;%dH", row + 1, col + 1);
}

static void ansi_ed_refresh(void) {
    struct ed_buffer *b = BUF();
    if (b->cy < b->scroll) b->scroll = b->cy;
    if (b->cy >= b->scroll + ED_VIEW_ROWS) b->scroll = b->cy - ED_VIEW_ROWS + 1;

    /* Top status bar */
    ansi_goto(0, 0);
    kprintf("\x1b[7m");
    kprintf("EDIT: %s%s  [Buf %d/%d]",
            b->filename, b->dirty ? " *" : "  ",
            ed_cur_buf + 1, ED_MAX_BUFS);
    int used = 6 + (int)strlen(b->filename) + 10;
    for (int i = used; i < VGA_WIDTH; i++) kprintf(" ");
    kprintf("\x1b[0m");

    /* Content lines */
    for (int r = 0; r < ED_VIEW_ROWS; r++) {
        ansi_goto(r + 1, 0);
        int line = b->scroll + r;
        if (line < b->num_lines) {
            kprintf("%s\x1b[K", b->lines[line]);
        } else {
            kprintf("\x1b[34m~\x1b[0m\x1b[K");
        }
    }

    /* Bottom status bar */
    ansi_goto(VGA_HEIGHT - 1, 0);
    kprintf("\x1b[7m");
    const char *help = "^S:Save ^Q:Quit ^X:Cut ^C:Copy ^V:Paste ^N:Next ^P:Prev";
    kprintf("%s", help);
    int help_len = (int)strlen(help);

    /* Line:Col on right */
    char pos[16]; int pn = 0;
    char tmp[8]; int ti = 0;
    int v = b->cy + 1;
    if (!v) { tmp[ti++] = '0'; } else { while (v) { tmp[ti++] = '0' + v%10; v /= 10; } }
    for (int i = ti-1; i >= 0; i--) pos[pn++] = tmp[i];
    pos[pn++] = ':'; ti = 0;
    v = b->cx + 1;
    if (!v) { tmp[ti++] = '0'; } else { while (v) { tmp[ti++] = '0' + v%10; v /= 10; } }
    for (int i = ti-1; i >= 0; i--) pos[pn++] = tmp[i];
    pos[pn] = '\0';
    int pad = VGA_WIDTH - help_len - pn - 1;
    if (pad > 0) for (int i = 0; i < pad; i++) kprintf(" ");
    kprintf(" %s\x1b[0m", pos);

    /* Position cursor */
    int scr_col = b->cx < VGA_WIDTH ? b->cx : VGA_WIDTH - 1;
    ansi_goto(b->cy - b->scroll + 1, scr_col);

    kprintf_flush();
}

static void ed_refresh(void) {
    if (ed_telnet_mode) ansi_ed_refresh();
    else                vga_ed_refresh();
}

/* ── Buffer operations ───────────────────────────────────────────── */

static void ed_insert_char(char c) {
    struct ed_buffer *b = BUF();
    int len = ed_line_len(b->cy);
    if (len >= ED_LINE_LEN - 1) return;
    for (int i = len; i > b->cx; i--)
        b->lines[b->cy][i] = b->lines[b->cy][i - 1];
    b->lines[b->cy][b->cx] = c;
    b->lines[b->cy][len + 1] = '\0';
    b->cx++;
    b->dirty = 1;
}

static void ed_insert_newline(void) {
    struct ed_buffer *b = BUF();
    if (b->num_lines >= ED_MAX_LINES) return;
    for (int i = b->num_lines; i > b->cy + 1; i--)
        memcpy(b->lines[i], b->lines[i - 1], ED_LINE_LEN);
    b->num_lines++;
    int len = ed_line_len(b->cy);
    int tail = len - b->cx;
    memcpy(b->lines[b->cy + 1], b->lines[b->cy] + b->cx, (size_t)tail);
    b->lines[b->cy + 1][tail] = '\0';
    b->lines[b->cy][b->cx] = '\0';
    b->cy++;
    b->cx = 0;
    b->dirty = 1;
}

static void ed_backspace(void) {
    struct ed_buffer *b = BUF();
    if (b->cx > 0) {
        int len = ed_line_len(b->cy);
        for (int i = b->cx - 1; i < len; i++)
            b->lines[b->cy][i] = b->lines[b->cy][i + 1];
        b->cx--;
        b->dirty = 1;
    } else if (b->cy > 0) {
        int prev_len = ed_line_len(b->cy - 1);
        int cur_len  = ed_line_len(b->cy);
        if (prev_len + cur_len < ED_LINE_LEN) {
            memcpy(b->lines[b->cy - 1] + prev_len, b->lines[b->cy], (size_t)(cur_len + 1));
            for (int i = b->cy; i < b->num_lines - 1; i++)
                memcpy(b->lines[i], b->lines[i + 1], ED_LINE_LEN);
            b->num_lines--;
            memset(b->lines[b->num_lines], 0, ED_LINE_LEN);
            b->cy--;
            b->cx = prev_len;
            b->dirty = 1;
        }
    }
}

static void ed_delete_char(void) {
    struct ed_buffer *b = BUF();
    int len = ed_line_len(b->cy);
    if (b->cx < len) {
        for (int i = b->cx; i < len; i++)
            b->lines[b->cy][i] = b->lines[b->cy][i + 1];
        b->dirty = 1;
    } else if (b->cy < b->num_lines - 1) {
        int next_len = ed_line_len(b->cy + 1);
        if (len + next_len < ED_LINE_LEN) {
            memcpy(b->lines[b->cy] + len, b->lines[b->cy + 1], (size_t)(next_len + 1));
            for (int i = b->cy + 1; i < b->num_lines - 1; i++)
                memcpy(b->lines[i], b->lines[i + 1], ED_LINE_LEN);
            b->num_lines--;
            memset(b->lines[b->num_lines], 0, ED_LINE_LEN);
            b->dirty = 1;
        }
    }
}

/* ── Clipboard operations ────────────────────────────────────────── */

/* Copy current line to clipboard */
static void ed_yank_line(void) {
    struct ed_buffer *b = BUF();
    int len = ed_line_len(b->cy);
    int copy_len = len < ED_CLIP_LEN - 1 ? len : ED_CLIP_LEN - 1;
    memcpy(ed_clipboard, b->lines[b->cy], (size_t)copy_len);
    ed_clipboard[copy_len] = '\0';
    ed_clip_len = copy_len;
}

/* Cut (delete + yank) current line */
static void ed_cut_line(void) {
    struct ed_buffer *b = BUF();
    ed_yank_line();
    if (b->num_lines <= 1) {
        /* Only one line: just clear it */
        b->lines[0][0] = '\0';
        b->cx = 0;
        b->dirty = 1;
        return;
    }
    for (int i = b->cy; i < b->num_lines - 1; i++)
        memcpy(b->lines[i], b->lines[i + 1], ED_LINE_LEN);
    b->num_lines--;
    memset(b->lines[b->num_lines], 0, ED_LINE_LEN);
    if (b->cy >= b->num_lines) b->cy = b->num_lines - 1;
    if (b->cx > ed_line_len(b->cy)) b->cx = ed_line_len(b->cy);
    b->dirty = 1;
}

/* Paste clipboard below current line */
static void ed_paste_below(void) {
    struct ed_buffer *b = BUF();
    if (ed_clip_len == 0 || b->num_lines >= ED_MAX_LINES) return;
    int paste_line = b->cy + 1;
    if (paste_line > b->num_lines) paste_line = b->num_lines;
    for (int i = b->num_lines; i > paste_line; i--)
        memcpy(b->lines[i], b->lines[i - 1], ED_LINE_LEN);
    memcpy(b->lines[paste_line], ed_clipboard, (size_t)ed_clip_len);
    b->lines[paste_line][ed_clip_len] = '\0';
    b->num_lines++;
    b->cy = paste_line;
    b->cx = 0;
    b->dirty = 1;
}

/* Paste clipboard above current line */
static void ed_paste_above(void) {
    struct ed_buffer *b = BUF();
    if (ed_clip_len == 0 || b->num_lines >= ED_MAX_LINES) return;
    for (int i = b->num_lines; i > b->cy; i--)
        memcpy(b->lines[i], b->lines[i - 1], ED_LINE_LEN);
    memcpy(b->lines[b->cy], ed_clipboard, (size_t)ed_clip_len);
    b->lines[b->cy][ed_clip_len] = '\0';
    b->num_lines++;
    b->cx = 0;
    b->dirty = 1;
}

/* ── Multi-buffer switching ──────────────────────────────────────── */

/* Try to open a command like ":e filename" or ":bn" / ":bp" */
static void ed_handle_command(const char *input) {
    struct ed_buffer *b = BUF();
    /* Skip leading whitespace */
    while (*input == ' ') input++;

    if (strncmp(input, ":e ", 3) == 0) {
        /* :e filename — switch to next buffer and open file */
        const char *fname = input + 3;
        while (*fname == ' ') fname++;
        if (!*fname) return;

        /* Find next unused buffer or cycle to next */
        int target = ed_cur_buf;
        for (int i = 1; i < ED_MAX_BUFS; i++) {
            int candidate = (ed_cur_buf + i) % ED_MAX_BUFS;
            if (!ed_buffers[candidate].in_use) {
                target = candidate;
                break;
            }
        }
        /* If all in use, overwrite the next buffer */
        if (target == ed_cur_buf)
            target = (ed_cur_buf + 1) % ED_MAX_BUFS;

        /* Save old buffer state partly (already in place), init new */
        ed_cur_buf = target;
        b = BUF();
        memset(b->lines, 0, sizeof(b->lines));
        b->num_lines = 1;
        b->cx = 0; b->cy = 0; b->scroll = 0;
        b->dirty = 0;
        b->in_use = 1;
        strncpy(b->filename, fname, sizeof(b->filename) - 1);
        b->filename[sizeof(b->filename) - 1] = '\0';

        /* Load the file into this buffer */
        char path[66];
        if (b->filename[0] != '/') { path[0] = '/'; strcpy(path + 1, b->filename); }
        else strcpy(path, b->filename);

        char buf[ED_MAX_LINES * ED_LINE_LEN];
        uint32_t size = 0;
        if (fs_read_file(path, buf, (int)sizeof(buf) - 1, &size) == 0 && size > 0) {
            buf[size] = '\0';
            b->num_lines = 0;
            int col = 0;
            for (uint32_t i = 0; i < size && b->num_lines < ED_MAX_LINES; i++) {
                if (buf[i] == '\n') {
                    b->lines[b->num_lines][col] = '\0';
                    b->num_lines++;
                    col = 0;
                } else if (col < ED_LINE_LEN - 1) {
                    b->lines[b->num_lines][col++] = buf[i];
                }
            }
            if (col > 0 || b->num_lines == 0) {
                b->lines[b->num_lines][col] = '\0';
                b->num_lines++;
            }
        }
        return;
    }

    if (strcmp(input, ":bn") == 0) {
        /* Next buffer */
        for (int i = 1; i < ED_MAX_BUFS; i++) {
            int candidate = (ed_cur_buf + i) % ED_MAX_BUFS;
            if (ed_buffers[candidate].in_use) {
                ed_cur_buf = candidate;
                return;
            }
        }
        return;
    }

    if (strcmp(input, ":bp") == 0) {
        /* Previous buffer */
        for (int i = 1; i < ED_MAX_BUFS; i++) {
            int candidate = (ed_cur_buf - i + ED_MAX_BUFS) % ED_MAX_BUFS;
            if (ed_buffers[candidate].in_use) {
                ed_cur_buf = candidate;
                return;
            }
        }
        return;
    }
}

/* Read a single line from input for command mode */
static void ed_read_command(char *buf, int max) {
    int pos = 0;
    buf[0] = '\0';
    for (;;) {
        int k = ed_getkey();
        if (k == '\n' || k == '\r') {
            buf[pos] = '\0';
            return;
        }
        if ((k == '\b' || k == 127) && pos > 0) {
            pos--;
            if (ed_telnet_mode) kprintf("\b \b");
            continue;
        }
        if (k >= 32 && k < 127 && pos < max - 1) {
            buf[pos++] = (char)k;
            if (ed_telnet_mode) kprintf("%c", (char)k);
        }
    }
}

/* ── Save / Load ─────────────────────────────────────────────────── */

static void ed_save(void) {
    struct ed_buffer *b = BUF();
    char buf[ED_MAX_LINES * ED_LINE_LEN];
    int pos = 0;
    for (int i = 0; i < b->num_lines; i++) {
        int len = ed_line_len(i);
        if (pos + len + 1 >= (int)sizeof(buf)) {
            /* Try to save partial file */
            if (pos == 0) break;
            /* Truncate by ending with newline marker */
            break;
        }
        memcpy(buf + pos, b->lines[i], (size_t)len);
        pos += len;
        if (i < b->num_lines - 1) buf[pos++] = '\n';
    }
    char path[66];
    if (b->filename[0] != '/') { path[0] = '/'; strcpy(path + 1, b->filename); }
    else strcpy(path, b->filename);

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
        b->dirty = 0;
    }
}

static void ed_load(struct ed_buffer *b) {
    char path[66];
    if (b->filename[0] != '/') { path[0] = '/'; strcpy(path + 1, b->filename); }
    else strcpy(path, b->filename);

    char buf[ED_MAX_LINES * ED_LINE_LEN];
    uint32_t size = 0;

    memset(b->lines, 0, sizeof(b->lines));
    b->num_lines = 1;
    b->cx = 0; b->cy = 0; b->scroll = 0; b->dirty = 0;

    if (fs_read_file(path, buf, (int)sizeof(buf) - 1, &size) < 0) return; /* new file */
    buf[size] = '\0';

    b->num_lines = 0;
    int col = 0;
    for (uint32_t i = 0; i < size && b->num_lines < ED_MAX_LINES; i++) {
        if (buf[i] == '\n') {
            b->lines[b->num_lines][col] = '\0';
            b->num_lines++;
            col = 0;
        } else if (col < ED_LINE_LEN - 1) {
            b->lines[b->num_lines][col++] = buf[i];
        }
    }
    if (col > 0 || b->num_lines == 0) {
        b->lines[b->num_lines][col] = '\0';
        b->num_lines++;
    }
}

/* ── Main entry point ────────────────────────────────────────────── */

void editor_open(const char *filename) {
    if (!filename || !filename[0]) {
        kprintf("Usage: edit <filename>\n");
        return;
    }

    /* Find or allocate a buffer slot for this file */
    int slot = -1;

    /* Check if this file is already open in a buffer */
    for (int i = 0; i < ED_MAX_BUFS; i++) {
        if (ed_buffers[i].in_use && strcmp(ed_buffers[i].filename, filename) == 0) {
            slot = i;
            break;
        }
    }

    if (slot < 0) {
        /* Find empty slot */
        for (int i = 0; i < ED_MAX_BUFS; i++) {
            if (!ed_buffers[i].in_use) {
                slot = i;
                break;
            }
        }
    }

    if (slot < 0) {
        /* All slots full — reuse least recently used (simplify: overwrite current) */
        slot = ed_cur_buf;
    }

    /* Initialize the buffer if it's a new slot or reinitializing */
    if (slot != ed_cur_buf || !ed_buffers[slot].in_use) {
        ed_cur_buf = slot;
        struct ed_buffer *b = BUF();
        memset(b->lines, 0, sizeof(b->lines));
        b->num_lines = 1;
        b->cx = 0; b->cy = 0; b->scroll = 0;
        b->dirty = 0;
        b->in_use = 1;
        strncpy(b->filename, filename, sizeof(b->filename) - 1);
        b->filename[sizeof(b->filename) - 1] = '\0';
        ed_load(b);
    } else {
        /* Already the active buffer, just switch to it */
        ed_cur_buf = slot;
    }

    struct ed_buffer *b = BUF();

    /* Telnet mode if kprintf is currently hooked to a network session */
    void (*hook)(char,void*) = 0; void *hctx = 0;
    kprintf_get_hook(&hook, &hctx);
    ed_telnet_mode = (hook != 0);

    /* Reset input ring */
    ed_input_head = ed_input_tail = 0;

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
            if (b->cy > 0) {
                b->cy--;
                if (b->cx > ed_line_len(b->cy)) b->cx = ed_line_len(b->cy);
            }
            break;
        case (unsigned char)KEY_DOWN:
            if (b->cy < b->num_lines - 1) {
                b->cy++;
                if (b->cx > ed_line_len(b->cy)) b->cx = ed_line_len(b->cy);
            }
            break;
        case (unsigned char)KEY_LEFT:
            if (b->cx > 0) b->cx--;
            else if (b->cy > 0) { b->cy--; b->cx = ed_line_len(b->cy); }
            break;
        case (unsigned char)KEY_RIGHT:
            if (b->cx < ed_line_len(b->cy)) b->cx++;
            else if (b->cy < b->num_lines - 1) { b->cy++; b->cx = 0; }
            break;
        case (unsigned char)KEY_PAGEUP:
            b->cy -= ED_VIEW_ROWS;
            if (b->cy < 0) b->cy = 0;
            if (b->cx > ed_line_len(b->cy)) b->cx = ed_line_len(b->cy);
            break;
        case (unsigned char)KEY_PAGEDOWN:
            b->cy += ED_VIEW_ROWS;
            if (b->cy >= b->num_lines) b->cy = b->num_lines - 1;
            if (b->cx > ed_line_len(b->cy)) b->cx = ed_line_len(b->cy);
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
        case 24: /* Ctrl-X: cut line */
            ed_cut_line();
            break;
        case 3:  /* Ctrl-C: copy line */
            ed_yank_line();
            break;
        case 22: /* Ctrl-V: paste below */
            ed_paste_below();
            break;
        case 25: /* Ctrl-Y: yank (paste above) */
            ed_paste_above();
            break;
        case 14: /* Ctrl-N: next buffer */
            {
                int next = (ed_cur_buf + 1) % ED_MAX_BUFS;
                if (ed_buffers[next].in_use) {
                    ed_cur_buf = next;
                    b = BUF();
                }
            }
            break;
        case 16: /* Ctrl-P: previous buffer */
            {
                int prev = (ed_cur_buf - 1 + ED_MAX_BUFS) % ED_MAX_BUFS;
                if (ed_buffers[prev].in_use) {
                    ed_cur_buf = prev;
                    b = BUF();
                }
            }
            break;
        case 6:  /* Ctrl-F: command prompt (:e, :bn, :bp) */
            {
                int was_telnet = ed_telnet_mode;
                if (was_telnet) {
                    kprintf("\x1b[%d;1H\x1b[7m : \x1b[0m", VGA_HEIGHT);
                } else {
                    uint8_t color = VGA_BLACK | (VGA_LIGHT_GREY << 4);
                    for (int c = 0; c < VGA_WIDTH; c++)
                        vga_put_entry_at(' ', VGA_LIGHT_GREY | (VGA_BLACK << 4), VGA_HEIGHT - 1, c);
                    vga_put_entry_at(':', color, VGA_HEIGHT - 1, 0);
                    vga_put_entry_at(' ', color, VGA_HEIGHT - 1, 1);
                    vga_set_cursor(VGA_HEIGHT - 1, 2);
                }
                char cmd[64];
                ed_read_command(cmd, (int)sizeof(cmd));
                ed_handle_command(cmd);
                b = BUF(); /* buffer may have changed */
            }
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

/* editor.c — Dual-mode text editor: VGA console + ANSI/telnet
 *
 * Features:
 *   - Edit up to 1024 lines per buffer (dynamically allocated line buffer)
 *   - Multi-buffer: up to 4 simultaneously open buffers
 *   - Clipboard: cut (Ctrl-X), copy (Ctrl-C), paste (Ctrl-V)
 *   - Yank whole line (Ctrl-Y), delete line (Alt-D)
 *   - Buffer switching: Ctrl-N (next), Ctrl-P (prev)
 *   - :e filename, :bn, :bp in command mode
 *   - /pattern forward search, ?pattern backward search
 *   - n / N repeat last search forward/backward
 *   - :%s/old/new/g global replace
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
#include "syntax.h"

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
    syntax_lang_t lang;      /* detected language for syntax highlighting */
    int   in_multi;          /* multi-line comment/string state for syntax */
};

static struct ed_buffer ed_buffers[ED_MAX_BUFS];
static int              ed_cur_buf = 0;  /* index of active buffer */

/* ── Clipboard ───────────────────────────────────────────────────── */
static char ed_clipboard[ED_CLIP_LEN];
static int  ed_clip_len = 0;

/* ── Search state ────────────────────────────────────────────────── */
static char ed_search_pattern[64] = "";
static int  ed_search_backward    = 0;   /* 1 = last search was backward */
static int  ed_search_last_line   = -1;  /* line of last match (-1 = none) */
static int  ed_search_last_col    = -1;  /* column of last match (-1 = none) */

/* ── Forward declarations ────────────────────────────────────────── */
static void ed_insert_char(char c);
static void ed_insert_newline(void);
static void ed_backspace(void);
static void ed_delete_char(void);
static void ed_yank_line(void);
static void ed_cut_line(void);
static void ed_paste_below(void);
static void ed_paste_above(void);
static void ed_handle_command(const char *input);
static void ed_read_command(char *buf, int max);
static void ed_save(void);
static void ed_load(struct ed_buffer *b);
static int  ed_find_next(const char *pattern, int start_line, int start_col,
                         int backward, int *out_line, int *out_col);
static int  ed_do_search(const char *pattern, int backward);
static int  ed_global_replace(const char *old_str, const char *new_str);

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
    uint8_t default_color = VGA_LIGHT_GREY | (VGA_BLACK << 4);
    /* Syntax highlighting state */
    syntax_token_t stokens[SYN_LINE_MAX];
    int s_multi = 0;

    for (int r = 0; r < ED_VIEW_ROWS; r++) {
        int line = b->scroll + r;
        int scr_row = r + 1;
        if (line < b->num_lines) {
            int len = (int)strlen(b->lines[line]);
            if (len > VGA_WIDTH) len = VGA_WIDTH;

            if (b->lang != SYNTAX_NONE) {
                /* Tokenize with language-aware highlighting */
                if (line == b->scroll + r) {
                    /* Re-tokenize from start of visible region to maintain
                     * multi-line state; for C we need to track slash-asterisk */
                    s_multi = 0;
                    for (int tl = 0; tl <= line; tl++) {
                        int llen = (int)strlen(b->lines[tl]);
                        if (llen > SYN_LINE_MAX) llen = SYN_LINE_MAX;
                        if (tl == line) {
                            syntax_tokenize(b->lang, b->lines[tl], llen,
                                           stokens, &s_multi);
                        } else {
                            syntax_tokenize(b->lang, b->lines[tl], llen,
                                           NULL, &s_multi);
                        }
                    }
                } else {
                    int llen = (int)strlen(b->lines[line]);
                    if (llen > SYN_LINE_MAX) llen = SYN_LINE_MAX;
                    syntax_tokenize(b->lang, b->lines[line], llen,
                                   stokens, &s_multi);
                }

                int c;
                for (c = 0; c < len; c++) {
                    uint8_t color = syntax_token_to_vga(
                        (syntax_token_t)(c < SYN_LINE_MAX ? stokens[c] : TOKEN_DEFAULT));
                    vga_put_entry_at(b->lines[line][c], color, scr_row, c);
                }
                for (; c < VGA_WIDTH; c++)
                    vga_put_entry_at(' ', default_color, scr_row, c);
            } else {
                /* No syntax highlighting — plain text */
                int c;
                for (c = 0; c < len; c++)
                    vga_put_entry_at(b->lines[line][c], default_color, scr_row, c);
                for (; c < VGA_WIDTH; c++)
                    vga_put_entry_at(' ', default_color, scr_row, c);
            }
        } else {
            vga_put_entry_at('~', VGA_DARK_GREY | (VGA_BLACK << 4), scr_row, 0);
            for (int c = 1; c < VGA_WIDTH; c++)
                vga_put_entry_at(' ', default_color, scr_row, c);
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
            if (b->lang != SYNTAX_NONE) {
                /* Syntax-highlighted ANSI output */
                syntax_token_t stokens2[SYN_LINE_MAX];
                int s_multi2 = 0;
                /* Re-tokenize from visible start for multi-line state */
                for (int tl = b->scroll; tl <= line; tl++) {
                    int llen = (int)strlen(b->lines[tl]);
                    if (llen > SYN_LINE_MAX) llen = SYN_LINE_MAX;
                    if (tl == line) {
                        syntax_tokenize(b->lang, b->lines[tl], llen,
                                       stokens2, &s_multi2);
                    } else {
                        syntax_tokenize(b->lang, b->lines[tl], llen,
                                       NULL, &s_multi2);
                    }
                }
                int len = (int)strlen(b->lines[line]);
                if (len > VGA_WIDTH) len = VGA_WIDTH;
                syntax_token_t prev_tok = TOKEN_DEFAULT;
                for (int c = 0; c < len; c++) {
                    syntax_token_t tok = (c < SYN_LINE_MAX) ? stokens2[c] : TOKEN_DEFAULT;
                    if (tok != prev_tok) {
                        kprintf("%s", syntax_token_to_ansi(tok));
                        prev_tok = tok;
                    }
                    kprintf("%c", b->lines[line][c]);
                }
                kprintf("%s\x1b[K", SYN_ANSI_RESET);
            } else {
                kprintf("%s\x1b[K", b->lines[line]);
            }
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
        b->lang = syntax_detect(fname);
        b->in_multi = 0;
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

    /* ── Search and replace: :%s/old/new/g ─────────────────────── */
    if (strncmp(input, ":%s", 3) == 0) {
        const char *p = input + 3;
        if (*p == '/') {
            p++; /* skip first delimiter */
            /* Extract old pattern */
            char old_str[64], new_str[64];
            int olen = 0;
            while (*p && *p != '/' && olen < (int)sizeof(old_str) - 1) {
                old_str[olen++] = *p++;
            }
            old_str[olen] = '\0';
            if (*p == '/') {
                p++; /* skip delimiter */
                int nlen = 0;
                while (*p && *p != '/' && *p != 'g' && nlen < (int)sizeof(new_str) - 1) {
                    new_str[nlen++] = *p++;
                }
                new_str[nlen] = '\0';
                /* Skip optional 'g' flag */
                if (*p == 'g') p++;

                int count = ed_global_replace(old_str, new_str);
                /* Show result on status line */
                if (ed_telnet_mode) {
                    kprintf("\\x1b[%d;1H\\x1b[44m Replaced %d occurrence(s) \\x1b[0m", VGA_HEIGHT, count);
                } else {
                    uint8_t c = VGA_WHITE | (VGA_DARK_GREY << 4);
                    char msg[48];
                    int n = snprintf(msg, sizeof(msg), " Replaced %d occurrence(s) ", count);
                    for (int i = 0; i < n && i < VGA_WIDTH; i++)
                        vga_put_entry_at(msg[i], c, VGA_HEIGHT - 1, i);
                }
                return;
            }
        }
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

/* ── Search and Replace ──────────────────────────────────────────── */

/* Find next occurrence of pattern starting from (start_line, start_col).
 * If backward is 0, search forward; if 1, search backward.
 * Returns 1 if found, with *out_line and *out_col set.
 * Searches from (start_line, start_col) forward/backward, wrapping
 * around the entire buffer. */
static int ed_find_next(const char *pattern, int start_line, int start_col,
                        int backward, int *out_line, int *out_col)
{
    struct ed_buffer *b = BUF();
    if (!pattern || !pattern[0] || b->num_lines <= 0)
        return 0;

    int pat_len = (int)strlen(pattern);
    int total_lines = b->num_lines;

    if (backward) {
        /* Search backward from (start_line, start_col) */
        int line = start_line;
        int col  = start_col;

        while (1) {
            /* On each line, search backward from current column */
            int line_len = (int)strlen(b->lines[line]);

            /* Start searching from col-1 downward to 0 */
            int end = (line == start_line) ? col : line_len;
            for (int c = end - pat_len; c >= 0; c--) {
                if (memcmp(b->lines[line] + c, pattern, (size_t)pat_len) == 0) {
                    *out_line = line;
                    *out_col  = c;
                    return 1;
                }
            }

            /* Move to previous line */
            line--;
            if (line < 0) {
                /* Wrap around to last line */
                line = total_lines - 1;
            }
            /* If we wrapped back to start_line, we've searched everything */
            if (line == start_line) {
                /* Search start_line from the end */
                col = (int)strlen(b->lines[line]);
                /* If col <= start_col means we already searched this line above */
                if (col <= start_col) break;
                /* Otherwise continue at col for this line */
                continue;
            }
            col = (int)strlen(b->lines[line]); /* search entire line */
        }
    } else {
        /* Search forward from (start_line, start_col) */
        int line = start_line;
        int col  = start_col;

        while (1) {
            int line_len = (int)strlen(b->lines[line]);

            /* Search from current position forward */
            int start_c = (line == start_line) ? col : 0;
            for (int c = start_c; c <= line_len - pat_len; c++) {
                if (memcmp(b->lines[line] + c, pattern, (size_t)pat_len) == 0) {
                    *out_line = line;
                    *out_col  = c;
                    return 1;
                }
            }

            /* Move to next line */
            line = (line + 1) % total_lines;
            /* If we wrapped back to start_line, we've searched everything */
            if (line == start_line) {
                /* Check start_line from the beginning up to col */
                for (int c = 0; c <= col - pat_len && c < start_c; c++) {
                    if (memcmp(b->lines[line] + c, pattern, (size_t)pat_len) == 0) {
                        *out_line = line;
                        *out_col  = c;
                        return 1;
                    }
                }
                break;
            }
        }
    }

    return 0; /* not found */
}

/* Search for a pattern entered by the user.
 * mode: 0 = forward (/), 1 = backward (?)
 * Returns 1 if found and cursor moved. */
static int ed_do_search(const char *pattern, int backward)
{
    struct ed_buffer *b = BUF();
    int match_line, match_col;

    /* Start searching from cursor position */
    int start_line = b->cy;
    int start_col  = b->cx;

    if (ed_find_next(pattern, start_line, start_col, backward,
                     &match_line, &match_col))
    {
        b->cy = match_line;
        b->cx = match_col;
        strncpy(ed_search_pattern, pattern, sizeof(ed_search_pattern) - 1);
        ed_search_pattern[sizeof(ed_search_pattern) - 1] = '\0';
        ed_search_backward  = backward;
        ed_search_last_line = match_line;
        ed_search_last_col  = match_col;
        return 1;
    }

    /* Try wrapping from the beginning/end of buffer */
    if (backward) {
        start_line = b->num_lines - 1;
        start_col  = (int)strlen(b->lines[start_line]);
    } else {
        start_line = 0;
        start_col  = 0;
    }

    if (ed_find_next(pattern, start_line, start_col, backward,
                     &match_line, &match_col))
    {
        b->cy = match_line;
        b->cx = match_col;
        strncpy(ed_search_pattern, pattern, sizeof(ed_search_pattern) - 1);
        ed_search_pattern[sizeof(ed_search_pattern) - 1] = '\0';
        ed_search_backward  = backward;
        ed_search_last_line = match_line;
        ed_search_last_col  = match_col;
        return 1;
    }

    return 0; /* not found anywhere */
}

/* Perform global replace: %s/old/new/g
 * Returns number of replacements made. */
static int ed_global_replace(const char *old_str, const char *new_str)
{
    struct ed_buffer *b = BUF();
    if (!old_str || !old_str[0] || !new_str) return 0;

    int old_len = (int)strlen(old_str);
    int new_len = (int)strlen(new_str);
    int count   = 0;

    for (int i = 0; i < b->num_lines; i++) {
        char *line = b->lines[i];
        int line_len = (int)strlen(line);
        char result[ED_LINE_LEN];
        int  pos = 0;
        int  cp  = 0; /* copy pointer into result */

        while (cp <= line_len - old_len) {
            /* Check for match at current position */
            if (memcmp(line + cp, old_str, (size_t)old_len) == 0) {
                /* Copy prefix up to match point */
                memcpy(result + pos, line + cp, (size_t)(cp - pos));
                pos += (cp - pos);
                /* Copy replacement string */
                if (pos + new_len < ED_LINE_LEN - 1) {
                    memcpy(result + pos, new_str, (size_t)new_len);
                    pos += new_len;
                }
                cp += old_len;
                count++;
                pos = cp; /* reset copy pointer */
            } else {
                cp++;
            }
        }
        /* Copy remaining characters */
        if (pos < line_len) {
            int rem = line_len - pos;
            if (pos + rem < ED_LINE_LEN - 1) {
                memcpy(result + pos, line + pos, (size_t)rem);
                pos += rem;
            }
        }
        result[pos] = '\0';

        if (count > 0) {
            memcpy(line, result, (size_t)(pos + 1));
            b->dirty = 1;
        }
    }

    return count;
}

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
        b->lang = syntax_detect(filename);
        b->in_multi = 0;
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
        case '/':  /* ── Forward search ── */
        case '?':  /* ── Backward search ── */
            {
                int backward = (k == '?');
                int was_telnet = ed_telnet_mode;

                /* Show search prompt on status line */
                if (was_telnet) {
                    kprintf("\x1b[%d;1H\x1b[7m %s \x1b[0m", VGA_HEIGHT,
                            backward ? "?" : "/");
                } else {
                    uint8_t color = VGA_BLACK | (VGA_LIGHT_GREY << 4);
                    for (int c = 0; c < VGA_WIDTH; c++)
                        vga_put_entry_at(' ', VGA_LIGHT_GREY | (VGA_BLACK << 4),
                                         VGA_HEIGHT - 1, c);
                    vga_put_entry_at(backward ? '?' : '/', color,
                                     VGA_HEIGHT - 1, 1);
                    vga_set_cursor(VGA_HEIGHT - 1, 2);
                }

                /* Read search pattern */
                char pattern[64];
                ed_read_command(pattern, (int)sizeof(pattern));

                if (pattern[0]) {
                    if (!ed_do_search(pattern, backward)) {
                        /* Pattern not found — show message */
                        if (was_telnet) {
                            kprintf("\x1b[%d;1H\x1b[41m Pattern not found: %s \x1b[0m",
                                    VGA_HEIGHT, pattern);
                        } else {
                            uint8_t c = VGA_WHITE | (VGA_RED << 4);
                            char msg[72];
                            snprintf(msg, sizeof(msg),
                                     " Pattern not found: %s ", pattern);
                            for (int i = 0; msg[i] && i < VGA_WIDTH; i++)
                                vga_put_entry_at(msg[i], c, VGA_HEIGHT - 1, i);
                        }
                    }
                }
                b = BUF(); /* re-read after potential search */
            }
            break;
        case 'n':  /* ── Repeat last search forward ── */
            if (ed_search_pattern[0]) {
                ed_do_search(ed_search_pattern, 0);
                b = BUF();
            }
            break;
        case 'N':  /* ── Repeat last search backward ── */
            if (ed_search_pattern[0]) {
                ed_do_search(ed_search_pattern, 1);
                b = BUF();
            }
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

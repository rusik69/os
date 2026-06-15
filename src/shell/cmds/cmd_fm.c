/*
 * cmd_fm.c — Visual file manager (two-pane directory browser)
 *
 * Usage:
 *   fm [path1] [path2]   — start with two panes showing given directories
 *   mc                    — alias for fm
 *
 * Controls:
 *   Tab         -- switch between left/right pane
 *   Up/Down     -- navigate file list
 *   Enter       -- enter selected directory
 *   Backspace   -- go to parent directory
 *   c           -- copy selected file/dir to other pane's current dir
 *   m           -- move selected file/dir to other pane's current dir
 *   r           -- rename selected file
 *   d           -- delete selected file/dir (with confirmation)
 *   s           -- cycle sort mode: name -> size -> mtime
 *   z           -- toggle sort direction
 *   l           -- refresh current pane
 *   q / Esc     -- quit
 *
 * This runs entirely in-kernel as a shell command, using the same
 * VFS/FS API exposed to all other built-in commands via libc.h.
 */

#include "shell_cmds.h"
#include "vga.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"     /* for qsort */
#include "vga.h"

/* ── Display layout ──────────────────────────────────────────────────── */

#define FM_PANEL_WIDTH      36          /* width of each file panel */
#define FM_DIVIDER_WIDTH     4          /* gap between panels */
#define FM_TOTAL_WIDTH      (FM_PANEL_WIDTH * 2 + FM_DIVIDER_WIDTH)
#define FM_LIST_LINES       19          /* lines available for file listing */
#define FM_HEADER_LINES      3          /* top: title bar, path bar, header row */
#define FM_FOOTER_LINES      2          /* bottom: status bar, key hints */

/* ── Limits ───────────────────────────────────────────────────────────── */

#define FM_MAX_ENTRIES      512         /* max directory entries per pane */
#define FM_MAX_PATH         256         /* max path length */
#define FM_MAX_NAME          FS_MAX_NAME /* 28 — same as kernel FS */
#define FM_CLIP_BUF         4096        /* copy/move buffer size */

/* ── Sort modes ───────────────────────────────────────────────────────── */

enum fm_sort_mode {
    FM_SORT_NAME = 0,
    FM_SORT_SIZE,
    FM_SORT_MTIME,
    FM_SORT_COUNT
};

static const char *fm_sort_names[] = { "Name", "Size", "Mtime" };

/* ── Directory entry (flat listing for one pane) ─────────────────────── */

struct fm_entry {
    char     name[FM_MAX_NAME];
    uint32_t size;
    uint8_t  type;              /* FS_TYPE_FILE, FS_TYPE_DIR, FS_TYPE_LINK */
    uint16_t mode;              /* permission bits */
    uint16_t uid;
    uint16_t gid;
    uint32_t mtime;
};

/* ── Pane state ───────────────────────────────────────────────────────── */

struct fm_pane {
    char             path[FM_MAX_PATH];       /* current directory */
    struct fm_entry  entries[FM_MAX_ENTRIES];
    int              entry_count;
    int              cursor;                  /* cursor index into entries[] */
    int              scroll;                  /* first visible entry index */
    uint64_t         total_size;              /* sum of file sizes in dir */
    int              file_count;              /* number of regular files */
    int              dir_count;               /* number of subdirectories */
    enum fm_sort_mode sort_mode;
    int              sort_descending;
};

/* ── Global state ─────────────────────────────────────────────────────── */

static struct fm_pane panes[2];   /* [0]=left, [1]=right */
static int active_pane = 0;       /* 0=left, 1=right */

static char fm_status_msg[64] = "";     /* one-line status message */
static int  fm_status_ticks = 0;        /* fade timer for status msg */

/* ── Forward declarations ─────────────────────────────────────────────── */

static void fm_draw(void);
static void fm_read_dir(struct fm_pane *pane);
static void fm_sort_entries(struct fm_pane *pane);
static void fm_enter_dir(struct fm_pane *pane, const char *name);
static void fm_parent_dir(struct fm_pane *pane);
static void fm_copy_file(struct fm_pane *src, struct fm_pane *dst, int idx);
static void fm_move_file(struct fm_pane *src, struct fm_pane *dst, int idx);
static void fm_delete_file(struct fm_pane *pane, int idx);
static void fm_rename_file(struct fm_pane *pane, int idx);

/* ── Comparison helpers for sorting ─────────────────────────────────── */

static int cmp_name_asc(const void *a, const void *b) {
    const struct fm_entry *ea = (const struct fm_entry *)a;
    const struct fm_entry *eb = (const struct fm_entry *)b;
    return strcmp(ea->name, eb->name);
}

static int cmp_name_desc(const void *a, const void *b) {
    return -cmp_name_asc(a, b);
}

static int cmp_size_asc(const void *a, const void *b) {
    const struct fm_entry *ea = (const struct fm_entry *)a;
    const struct fm_entry *eb = (const struct fm_entry *)b;
    if (ea->size < eb->size) return -1;
    if (ea->size > eb->size) return 1;
    return strcmp(ea->name, eb->name);
}

static int cmp_size_desc(const void *a, const void *b) {
    return -cmp_size_asc(a, b);
}

static int cmp_mtime_asc(const void *a, const void *b) {
    const struct fm_entry *ea = (const struct fm_entry *)a;
    const struct fm_entry *eb = (const struct fm_entry *)b;
    if (ea->mtime < eb->mtime) return -1;
    if (ea->mtime > eb->mtime) return 1;
    return strcmp(ea->name, eb->name);
}

static int cmp_mtime_desc(const void *a, const void *b) {
    return -cmp_mtime_asc(a, b);
}

/* ── Format size in human-readable form ───────────────────────────────── */

static const char *fmt_size(uint32_t bytes) {
    static char buf[16];
    if (bytes >= 1073741824u)
        snprintf(buf, sizeof(buf), "%u.%uG", bytes / 1073741824u,
                 (bytes % 1073741824u) / 107374182u);
    else if (bytes >= 1048576)
        snprintf(buf, sizeof(buf), "%u.%uM", bytes / 1048576,
                 (bytes % 1048576) / 104858);
    else if (bytes >= 1024)
        snprintf(buf, sizeof(buf), "%u.%uK", bytes / 1024,
                 (bytes % 1024) / 102);
    else
        snprintf(buf, sizeof(buf), "%uB", bytes);
    return buf;
}

/* ── File type icon ───────────────────────────────────────────────────── */

static const char *file_icon(uint8_t type) {
    switch (type) {
    case FS_TYPE_DIR:  return "D";
    case FS_TYPE_LINK: return "L";
    default:           return " ";
    }
}

/* ── Read directory into pane ──────────────────────────────────────────── */

static void fm_read_dir(struct fm_pane *pane) {
    char names[FM_MAX_ENTRIES][FS_MAX_NAME];
    int count = fs_list_names(pane->path, "", names, FM_MAX_ENTRIES);
    if (count < 0) count = 0;
    if (count > FM_MAX_ENTRIES) count = FM_MAX_ENTRIES;

    pane->entry_count = 0;
    pane->total_size = 0;
    pane->file_count = 0;
    pane->dir_count = 0;

    for (int i = 0; i < count; i++) {
        if (pane->entry_count >= FM_MAX_ENTRIES) break;

        /* Skip . and .. */
        if (strcmp(names[i], ".") == 0 || strcmp(names[i], "..") == 0)
            continue;

        /* Build full path */
        char full[FM_MAX_PATH];
        int plen = (int)strlen(pane->path);
        if (plen > 0 && pane->path[plen - 1] == '/')
            snprintf(full, sizeof(full), "%s%s", pane->path, names[i]);
        else
            snprintf(full, sizeof(full), "%s/%s", pane->path, names[i]);

        /* Stat the entry */
        struct fm_entry *e = &pane->entries[pane->entry_count];
        uint32_t fsize;
        uint8_t ftype;
        uint16_t fmode;
        uint16_t fuid;
        uint16_t fgid;

        int ret = fs_stat_ex(full, &fsize, &ftype, &fuid, &fgid, &fmode);
        if (ret != 0) {
            /* Try lstat if regular stat fails (e.g., broken symlink) */
            if (libc_fs_lstat(full, &fsize, &ftype) == 0) {
                ftype = FS_TYPE_LINK;
                fmode = 0;
                fuid  = 0;
                fgid  = 0;
            } else {
                continue;  /* skip entries we can't stat */
            }
        }

        strncpy(e->name, names[i], FM_MAX_NAME - 1);
        e->name[FM_MAX_NAME - 1] = '\0';
        e->size = fsize;
        e->type = ftype;
        e->mode = fmode;
        e->uid  = fuid;
        e->gid  = fgid;

        /* Get mtime via vfs_stat which has the mtime field */
        {
            struct vfs_stat vst;
            if (libc_vfs_stat(full, &vst) == 0) {
                e->mtime = vst.mtime;
            } else {
                e->mtime = 0;
            }
        }

        if (ftype == FS_TYPE_DIR) {
            pane->dir_count++;
        } else {
            pane->file_count++;
            pane->total_size += fsize;
        }

        pane->entry_count++;
    }

    /* Sort entries */
    fm_sort_entries(pane);

    /* Clamp cursor and scroll */
    if (pane->cursor >= pane->entry_count)
        pane->cursor = pane->entry_count > 0 ? pane->entry_count - 1 : 0;
    if (pane->cursor < 0) pane->cursor = 0;
    if (pane->scroll > pane->cursor)
        pane->scroll = pane->cursor;
    if (pane->cursor >= pane->scroll + FM_LIST_LINES)
        pane->scroll = pane->cursor - FM_LIST_LINES + 1;
}

/* ── Sort entries in a pane ────────────────────────────────────────────── */

static void fm_sort_entries(struct fm_pane *pane) {
    /* Choose comparator based on sort mode and direction */
    int (*cmp)(const void *, const void *);

    switch (pane->sort_mode) {
    case FM_SORT_SIZE:
        cmp = pane->sort_descending ? cmp_size_desc : cmp_size_asc;
        break;
    case FM_SORT_MTIME:
        cmp = pane->sort_descending ? cmp_mtime_desc : cmp_mtime_asc;
        break;
    default: /* FM_SORT_NAME */
        cmp = pane->sort_descending ? cmp_name_desc : cmp_name_asc;
        break;
    }

    qsort(pane->entries, pane->entry_count, sizeof(struct fm_entry), cmp);
}

/* ── Enter a subdirectory ──────────────────────────────────────────────── */

static void fm_enter_dir(struct fm_pane *pane, const char *name) {
    char new_path[FM_MAX_PATH];
    int plen = (int)strlen(pane->path);
    if (plen > 0 && pane->path[plen - 1] == '/')
        snprintf(new_path, sizeof(new_path), "%s%s", pane->path, name);
    else
        snprintf(new_path, sizeof(new_path), "%s/%s", pane->path, name);

    strncpy(pane->path, new_path, FM_MAX_PATH - 1);
    pane->path[FM_MAX_PATH - 1] = '\0';
    pane->cursor = 0;
    pane->scroll = 0;
    fm_read_dir(pane);
}

/* ── Go to parent directory ────────────────────────────────────────────── */

static void fm_parent_dir(struct fm_pane *pane) {
    char *p = pane->path + (int)strlen(pane->path) - 1;
    /* Strip trailing slash */
    while (p > pane->path && *p == '/') { *p = '\0'; p--; }
    /* Find last / */
    p = strrchr(pane->path, '/');
    if (p && p != pane->path) {
        *p = '\0';  /* truncate at parent */
    } else if (p == pane->path) {
        /* At root already — go to / */
        pane->path[1] = '\0';
    } else {
        strncpy(pane->path, "/", sizeof(pane->path) - 1);
        pane->path[sizeof(pane->path) - 1] = '\0';
    }
    pane->cursor = 0;
    pane->scroll = 0;
    fm_read_dir(pane);
}

/* ── Set status message (with auto-fade) ──────────────────────────────── */

static void fm_set_status(const char *msg) {
    strncpy(fm_status_msg, msg, sizeof(fm_status_msg) - 1);
    fm_status_msg[sizeof(fm_status_msg) - 1] = '\0';
    fm_status_ticks = 20;  /* ~20 redraw cycles */
}

/* ── Copy file from src pane to dst pane's current dir ───────────────── */

static void fm_copy_file(struct fm_pane *src, struct fm_pane *dst, int idx) {
    if (idx < 0 || idx >= src->entry_count) return;

    struct fm_entry *e = &src->entries[idx];

    /* Build source and destination paths */
    char src_path[FM_MAX_PATH];
    int splen = (int)strlen(src->path);
    if (splen > 0 && src->path[splen - 1] == '/')
        snprintf(src_path, sizeof(src_path), "%s%s", src->path, e->name);
    else
        snprintf(src_path, sizeof(src_path), "%s/%s", src->path, e->name);

    char dst_path[FM_MAX_PATH];
    int dplen = (int)strlen(dst->path);
    if (dplen > 0 && dst->path[dplen - 1] == '/')
        snprintf(dst_path, sizeof(dst_path), "%s%s", dst->path, e->name);
    else
        snprintf(dst_path, sizeof(dst_path), "%s/%s", dst->path, e->name);

    /* Read the file */
    uint8_t buf[FM_CLIP_BUF];
    uint32_t out_size = 0;
    if (fs_read_file(src_path, buf, sizeof(buf), &out_size) != 0) {
        fm_set_status("ERROR: cannot read source file");
        return;
    }

    /* Create and write the destination */
    if (fs_create(dst_path, FS_TYPE_FILE) != 0) {
        fm_set_status("ERROR: cannot create destination file");
        return;
    }
    if (fs_write_file(dst_path, buf, out_size) != 0) {
        fm_set_status("ERROR: cannot write destination file");
        return;
    }

    fm_set_status("Copied successfully");
    fm_read_dir(dst);  /* refresh destination pane */
}

/* ── Move file from src pane to dst pane's current dir ───────────────── */

static void fm_move_file(struct fm_pane *src, struct fm_pane *dst, int idx) {
    if (idx < 0 || idx >= src->entry_count) return;

    struct fm_entry *e = &src->entries[idx];

    /* Build source and destination paths */
    char src_path[FM_MAX_PATH];
    int splen = (int)strlen(src->path);
    if (splen > 0 && src->path[splen - 1] == '/')
        snprintf(src_path, sizeof(src_path), "%s%s", src->path, e->name);
    else
        snprintf(src_path, sizeof(src_path), "%s/%s", src->path, e->name);

    char dst_path[FM_MAX_PATH];
    int dplen = (int)strlen(dst->path);
    if (dplen > 0 && dst->path[dplen - 1] == '/')
        snprintf(dst_path, sizeof(dst_path), "%s%s", dst->path, e->name);
    else
        snprintf(dst_path, sizeof(dst_path), "%s/%s", dst->path, e->name);

    /* Move = copy + delete */
    uint8_t buf[FM_CLIP_BUF];
    uint32_t out_size = 0;
    if (fs_read_file(src_path, buf, sizeof(buf), &out_size) != 0) {
        fm_set_status("ERROR: cannot read source file for move");
        return;
    }

    if (fs_create(dst_path, FS_TYPE_FILE) != 0) {
        fm_set_status("ERROR: cannot create destination for move");
        return;
    }
    if (fs_write_file(dst_path, buf, out_size) != 0) {
        fm_set_status("ERROR: cannot write destination for move");
        return;
    }

    /* Delete source */
    if (fs_delete(src_path) != 0) {
        fm_set_status("ERROR: cannot delete source after copy (file may be duplicated)");
        return;
    }

    fm_set_status("Moved successfully");
    fm_read_dir(src);  /* refresh source pane */
    fm_read_dir(dst);  /* refresh destination pane */
}

/* ── Delete file/dir with confirmation ────────────────────────────────── */

static void fm_delete_file(struct fm_pane *pane, int idx) {
    if (idx < 0 || idx >= pane->entry_count) return;

    struct fm_entry *e = &pane->entries[idx];

    /* Build full path */
    char full[FM_MAX_PATH];
    int plen = (int)strlen(pane->path);
    if (plen > 0 && pane->path[plen - 1] == '/')
        snprintf(full, sizeof(full), "%s%s", pane->path, e->name);
    else
        snprintf(full, sizeof(full), "%s/%s", pane->path, e->name);

    /* Show confirmation prompt inline (at bottom of screen) */
    char saved_status[64];
    strncpy(saved_status, fm_status_msg, sizeof(saved_status) - 1);
    saved_status[sizeof(saved_status) - 1] = '\0';

    vga_set_color(VGA_WHITE, VGA_RED);
    kprintf("\rDelete '%s'? (y/N): ", e->name);
    char confirm = keyboard_getchar();
    /* Clear the prompt line */
    kprintf("\r%*s\r", FM_TOTAL_WIDTH, "");

    if (confirm == 'y' || confirm == 'Y') {
        if (fs_delete(full) == 0) {
            fm_set_status("Deleted");
            fm_read_dir(pane);
        } else {
            fm_set_status("ERROR: cannot delete file");
        }
    } else {
        strncpy(fm_status_msg, saved_status, sizeof(fm_status_msg) - 1);
        fm_status_msg[sizeof(fm_status_msg) - 1] = '\0';
        fm_set_status("Delete cancelled");
    }
}

/* ── Rename file ──────────────────────────────────────────────────────── */

static void fm_rename_file(struct fm_pane *pane, int idx) {
    if (idx < 0 || idx >= pane->entry_count) return;

    struct fm_entry *e = &pane->entries[idx];

    /* Build current full path */
    char old_path[FM_MAX_PATH];
    int plen = (int)strlen(pane->path);
    if (plen > 0 && pane->path[plen - 1] == '/')
        snprintf(old_path, sizeof(old_path), "%s%s", pane->path, e->name);
    else
        snprintf(old_path, sizeof(old_path), "%s/%s", pane->path, e->name);

    /* Save current status */
    char saved_status[64];
    strncpy(saved_status, fm_status_msg, sizeof(saved_status) - 1);
    saved_status[sizeof(saved_status) - 1] = '\0';

    /* Prompt for new name */
    vga_set_color(VGA_WHITE, VGA_BLACK);
    kprintf("\rRename '%s' to: ", e->name);

    /* Read new name (simple line input) */
    char new_name[FM_MAX_NAME];
    int ni = 0;
    memset(new_name, 0, sizeof(new_name));

    for (;;) {
        char ch = keyboard_getchar();

        if (ch == '\r' || ch == '\n') {
            break;   /* confirm */
        }
        if (ch == 0x1B || ch == 'q') {
            /* Cancel */
            kprintf("\r%*s\r", FM_TOTAL_WIDTH, "");
            strncpy(fm_status_msg, saved_status, sizeof(fm_status_msg) - 1);
            fm_status_msg[sizeof(fm_status_msg) - 1] = '\0';
            fm_set_status("Rename cancelled");
            return;
        }
        if (ch == '\b' || ch == 127) {
            if (ni > 0) {
                ni--;
                kprintf("\b \b");
            }
            continue;
        }
        if (ch >= 32 && ch < 127 && ni < FM_MAX_NAME - 1) {
            new_name[ni++] = ch;
            kprintf("%c", ch);
        }
    }

    new_name[ni] = '\0';

    /* Clear prompt */
    kprintf("\r%*s\r", FM_TOTAL_WIDTH, "");

    if (ni == 0) {
        strncpy(fm_status_msg, saved_status, sizeof(fm_status_msg) - 1);
        fm_status_msg[sizeof(fm_status_msg) - 1] = '\0';
        fm_set_status("Rename cancelled (empty name)");
        return;
    }

    /* Build new path */
    char new_path[FM_MAX_PATH];
    if (plen > 0 && pane->path[plen - 1] == '/')
        snprintf(new_path, sizeof(new_path), "%s%s", pane->path, new_name);
    else
        snprintf(new_path, sizeof(new_path), "%s/%s", pane->path, new_name);

    /* Rename = copy + delete (since kernel FS doesn't have rename) */
    uint8_t buf[FM_CLIP_BUF];
    uint32_t out_size = 0;
    if (fs_read_file(old_path, buf, sizeof(buf), &out_size) == 0) {
        if (fs_create(new_path, FS_TYPE_FILE) == 0) {
            if (fs_write_file(new_path, buf, out_size) == 0) {
                if (fs_delete(old_path) == 0) {
                    fm_set_status("Renamed successfully");
                    fm_read_dir(pane);
                    return;
                }
            }
        }
    }

    /* If we get here, something failed — try to clean up partial new file */
    fs_delete(new_path);
    fm_set_status("ERROR: rename failed");
}

/* ── Draw one pane ─────────────────────────────────────────────────────── */

static void fm_draw_pane(struct fm_pane *pane, int is_active, int x_offset) {
    int x = x_offset;

    /* ── Title bar ──────────────────────────────────────────────────── */
    vga_set_cursor(0, x);
    if (is_active) {
        vga_set_color(VGA_WHITE, VGA_BLUE);
    } else {
        vga_set_color(VGA_LIGHT_GREY, VGA_DARK_GREY);
    }

    /* Draw pane title with sort indicator */
    const char *sort_name = fm_sort_names[pane->sort_mode];
    vga_set_cursor(0, x);
    kprintf(" %s [%s%c]  ",
            is_active ? "*" : " ",
            sort_name,
            pane->sort_descending ? '^' : 'v');
    /* Pad to full width */
    {
        int used = 2 + (int)strlen(sort_name) + 1 + 2 + 2;
        for (int i = used; i < FM_PANEL_WIDTH; i++)
            kprintf(" ");
    }

    /* ── Path bar ────────────────────────────────────────────────────── */
    vga_set_cursor(1, x);
    vga_set_color(VGA_CYAN, is_active ? VGA_BLUE : VGA_DARK_GREY);
    {
        char path_disp[FM_PANEL_WIDTH + 1];
        int path_len = (int)strlen(pane->path);
        if (path_len > FM_PANEL_WIDTH - 4) {
            snprintf(path_disp, sizeof(path_disp), "..%s",
                     pane->path + path_len - (FM_PANEL_WIDTH - 6));
        } else {
            snprintf(path_disp, sizeof(path_disp), "%s", pane->path);
        }
        kprintf(" %s", path_disp);
        int pd_len = (int)strlen(path_disp) + 1;
        for (int i = pd_len; i < FM_PANEL_WIDTH; i++)
            kprintf(" ");
    }

    /* ── Column headers ──────────────────────────────────────────────── */
    vga_set_cursor(2, x);
    vga_set_color(VGA_LIGHT_GREY, is_active ? VGA_BLUE : VGA_DARK_GREY);
    kprintf("  Name             Size     Mtime");
    for (int i = 30; i < FM_PANEL_WIDTH; i++)
        kprintf(" ");

    /* ── File listing ────────────────────────────────────────────────── */
    int lines_used = 0;
    for (int i = pane->scroll; i < pane->entry_count && lines_used < FM_LIST_LINES; i++) {
        struct fm_entry *e = &pane->entries[i];
        int row = FM_HEADER_LINES + lines_used;

        vga_set_cursor(row, x);

        /* Highlight active cursor row, inactive pane selected */
        if (i == pane->cursor) {
            if (is_active)
                vga_set_color(VGA_WHITE, VGA_DARK_GREY);
            else
                vga_set_color(VGA_LIGHT_GREY, VGA_DARK_GREY);
        } else {
            if (e->type == FS_TYPE_DIR)
                vga_set_color(VGA_CYAN, VGA_BLACK);
            else if (e->type == FS_TYPE_LINK)
                vga_set_color(VGA_YELLOW, VGA_BLACK);
            else
                vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        }

        /* File type icon */
        kprintf("%s ", file_icon(e->type));

        /* Name (truncated to fit) */
        int name_limit = FM_PANEL_WIDTH - 20;
        if ((int)strlen(e->name) > name_limit - 3) {
            char name_disp[64];
            strncpy(name_disp, e->name, name_limit - 5);
            name_disp[name_limit - 5] = '\0';
            kprintf("%s", name_disp);
            kprintf("..");
        } else {
            kprintf("%s", e->name);
        }

        /* Pad after name */
        int nlen = (int)strlen(e->name);
        if (nlen > name_limit) nlen = name_limit;
        for (int p = nlen + 2; p < name_limit; p++)  /* +2 for icon + space */
            kprintf(" ");

        /* Size (right-aligned) */
        if (e->type == FS_TYPE_DIR) {
            kprintf("  <DIR> ");
        } else {
            const char *sz = fmt_size(e->size);
            int sz_len = (int)strlen(sz);
            for (int p = sz_len; p < 7; p++) kprintf(" ");
            kprintf("%s", sz);
        }

        /* Mtime (seconds since boot) */
        kprintf(" %u", (unsigned int)e->mtime);

        lines_used++;
    }

    /* Fill remaining lines with blanks */
    while (lines_used < FM_LIST_LINES) {
        vga_set_cursor(FM_HEADER_LINES + lines_used, x);
        for (int i = 0; i < FM_PANEL_WIDTH; i++)
            kprintf(" ");
        lines_used++;
    }

    /* ── Summary line at bottom of pane ──────────────────────────────── */
    vga_set_cursor(FM_HEADER_LINES + FM_LIST_LINES, x);
    if (is_active)
        vga_set_color(VGA_WHITE, VGA_BLUE);
    else
        vga_set_color(VGA_LIGHT_GREY, VGA_DARK_GREY);
    {
        char summary[64];
        snprintf(summary, sizeof(summary), " %d items  %d dirs  %s ",
                 pane->entry_count, pane->dir_count,
                 pane->entry_count > 0 ? fmt_size((uint32_t)pane->total_size) : "0B");
        kprintf("%s", summary);
        int sl = (int)strlen(summary);
        for (int i = sl; i < FM_PANEL_WIDTH; i++)
            kprintf(" ");
    }
}

/* ── Draw the full screen ──────────────────────────────────────────────── */

static void fm_draw(void) {
    /* ── Draw left pane ─────────────────────────────────────────────── */
    fm_draw_pane(&panes[0], active_pane == 0, 0);

    /* ── Draw divider ────────────────────────────────────────────────── */
    int div_x = FM_PANEL_WIDTH;
    for (int row = 0; row < FM_HEADER_LINES + FM_LIST_LINES + 1; row++) {
        vga_set_cursor(row, div_x);
        vga_set_color(VGA_WHITE, VGA_BLUE);
        kprintf(" | ");
    }

    /* ── Draw right pane ────────────────────────────────────────────── */
    fm_draw_pane(&panes[1], active_pane == 1, FM_PANEL_WIDTH + FM_DIVIDER_WIDTH);

    /* ── Status message bar ──────────────────────────────────────────── */
    int status_row = FM_HEADER_LINES + FM_LIST_LINES + 1;
    vga_set_cursor(status_row, 0);
    if (fm_status_ticks > 0) {
        vga_set_color(VGA_WHITE, VGA_RED);
        kprintf("%s", fm_status_msg);
        fm_status_ticks--;
    } else {
        fm_status_msg[0] = '\0';
        /* Clear status area */
        vga_set_color(VGA_BLACK, VGA_BLACK);
        for (int i = 0; i < FM_TOTAL_WIDTH; i++)
            kprintf(" ");
    }

    /* ── Key hints footer ────────────────────────────────────────────── */
    int hint_row = status_row + 1;
    vga_set_cursor(hint_row, 0);
    vga_set_color(VGA_CYAN, VGA_BLACK);
    kprintf("Tab=switch  Up/Down=nav  Enter=open  <-=parent  ");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    kprintf("c=copy  m=move  r=rename  d=delete  s=sort  q=quit");

    /* Clear rest of hint line */
    for (int i = 68; i < FM_TOTAL_WIDTH; i++)
        kprintf(" ");

    /* Move cursor to end to avoid visual artifacts */
    vga_set_cursor(FM_HEADER_LINES + FM_LIST_LINES + FM_FOOTER_LINES + 1, 0);
}

/* ── Cycle sort mode ──────────────────────────────────────────────────── */

static void fm_cycle_sort(struct fm_pane *pane) {
    pane->sort_mode = (enum fm_sort_mode)((pane->sort_mode + 1) % FM_SORT_COUNT);
    fm_sort_entries(pane);
    fm_set_status(fm_sort_names[pane->sort_mode]);
}

/* ── Main interactive loop ────────────────────────────────────────────── */

static void fm_main(const char *path1, const char *path2) {
    /* Initialise left pane */
    strncpy(panes[0].path, path1 ? path1 : "/", FM_MAX_PATH - 1);
    panes[0].path[FM_MAX_PATH - 1] = '\0';
    panes[0].cursor = 0;
    panes[0].scroll = 0;
    panes[0].sort_mode = FM_SORT_NAME;
    panes[0].sort_descending = 0;
    fm_read_dir(&panes[0]);

    /* Initialise right pane */
    strncpy(panes[1].path, path2 ? path2 : "/", FM_MAX_PATH - 1);
    panes[1].path[FM_MAX_PATH - 1] = '\0';
    panes[1].cursor = 0;
    panes[1].scroll = 0;
    panes[1].sort_mode = FM_SORT_NAME;
    panes[1].sort_descending = 0;
    fm_read_dir(&panes[1]);

    active_pane = 0;
    fm_status_msg[0] = '\0';
    fm_status_ticks = 0;

    /* Main keyboard loop */
    for (;;) {
        fm_draw();

        char ch = keyboard_getchar();

        struct fm_pane *cur = &panes[active_pane];

        switch (ch) {
        /* ── Navigation ──────────────────────────────────────────── */
        case '\t': {
            active_pane = 1 - active_pane;
            break;
        }

        case KEY_UP: {
            if (cur->cursor > 0) {
                cur->cursor--;
                if (cur->cursor < cur->scroll)
                    cur->scroll = cur->cursor;
            }
            break;
        }

        case KEY_DOWN: {
            if (cur->cursor < cur->entry_count - 1) {
                cur->cursor++;
                if (cur->cursor >= cur->scroll + FM_LIST_LINES)
                    cur->scroll = cur->cursor - FM_LIST_LINES + 1;
            }
            break;
        }

        case '\r':
        case '\n': {
            /* Enter directory */
            if (cur->cursor >= 0 && cur->cursor < cur->entry_count) {
                struct fm_entry *e = &cur->entries[cur->cursor];
                if (e->type == FS_TYPE_DIR) {
                    fm_enter_dir(cur, e->name);
                }
            }
            break;
        }

        case KEY_LEFT:
        case 127:   /* Backspace */
        case '\b': {
            fm_parent_dir(cur);
            break;
        }

        /* ── File operations ──────────────────────────────────────── */
        case 'c':
        case 'C': {
            /* Copy to other pane */
            struct fm_pane *other = &panes[1 - active_pane];
            fm_copy_file(cur, other, cur->cursor);
            break;
        }

        case 'm':
        case 'M': {
            /* Move to other pane */
            struct fm_pane *other = &panes[1 - active_pane];
            fm_move_file(cur, other, cur->cursor);
            break;
        }

        case 'd':
        case 'D': {
            fm_delete_file(cur, cur->cursor);
            break;
        }

        case 'r':
        case 'R': {
            fm_rename_file(cur, cur->cursor);
            break;
        }

        /* ── View options ─────────────────────────────────────────── */
        case 's':
        case 'S': {
            fm_cycle_sort(cur);
            break;
        }

        /* ── Sort direction ───────────────────────────────────────── */
        case 'z':
        case 'Z': {
            cur->sort_descending = !cur->sort_descending;
            fm_sort_entries(cur);
            fm_set_status(cur->sort_descending ? "Descending" : "Ascending");
            break;
        }

        /* ── Refresh current pane ─────────────────────────────────── */
        case 'l':
        case 'L': {
            fm_read_dir(cur);
            fm_set_status("Refreshed");
            break;
        }

        /* ── Quit ─────────────────────────────────────────────────── */
        case 'q':
        case 'Q':
        case 0x1B:  /* Escape */
            return;

        default:
            break;
        }
    }
}

/* ── Shell command entry point ─────────────────────────────────────────── */

void cmd_fm(const char *args) {
    /* Parse arguments: optionally two paths */
    char path1[FM_MAX_PATH] = "/";
    char path2[FM_MAX_PATH] = "/";

    if (args && *args) {
        char buf[256];
        strncpy(buf, args, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        /* First token */
        char *p = buf;
        while (*p == ' ') p++;
        char *tok1 = p;
        while (*p && *p != ' ') p++;
        if (*p) {
            *p++ = '\0';
            while (*p == ' ') p++;
            char *tok2 = p;
            if (*tok2) {
                strncpy(path2, tok2, FM_MAX_PATH - 1);
                path2[FM_MAX_PATH - 1] = '\0';
            }
        }
        if (*tok1) {
            strncpy(path1, tok1, FM_MAX_PATH - 1);
            path1[FM_MAX_PATH - 1] = '\0';
        }
    }

    fm_main(path1, path2);
}

/* ── Alias entry point for 'mc' command ───────────────────────────────── */

void cmd_mc(const char *args) {
    cmd_fm(args);
}

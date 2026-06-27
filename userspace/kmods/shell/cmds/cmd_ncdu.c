/*
 * cmd_ncdu.c — Disk Usage Analyzer (interactive ncdu-style)
 *
 * Scans a directory tree recursively, computes per-entry sizes, and
 * displays an interactive navigable view with bar graphs.  Supports
 * arrow-key navigation, directory entry/exit, and file deletion.
 *
 * Usage:  ncdu [path]
 *         If no path is given, start from the root "/".
 *
 * Reference: https://dev.yorhel.nl/ncdu  (original ncdu by Yoran Heling)
 */

#include "shell_cmds.h"
#include "vga.h"
#include "libc.h"
#include "printf.h"
#include "string.h"
#include "stdlib.h"     /* qsort */
#include "vga.h"

/* ── Limits ─────────────────────────────────────────────────────────── */

#define NCDU_MAX_ENTRIES     8192       /* total entries in scanned tree */
#define NCDU_MAX_NAME         64        /* max path component length */
#define NCDU_MAX_PATH        256        /* max full path length */
#define NCDU_DISPLAY_LINES    22        /* lines available for listing */


/* ── Tree entry ─────────────────────────────────────────────────────── */

struct ncdu_entry {
    char     name[NCDU_MAX_NAME];   /* filename (leaf only) */
    char     full_path[NCDU_MAX_PATH]; /* absolute path from root */
    uint64_t size;                   /* total size in bytes (cumulative for dirs) */
    uint64_t file_size;              /* this entry's own file size (0 for dirs) */
    uint8_t  type;                   /* 1=file, 2=dir */
    int      children;               /* index of first child in entries[] */
    int      nchildren;              /* number of children */
    int      depth;                  /* nesting depth from root */
    int      visited;                /* for DFS: 0=unvisited, 1=processing, 2=done */
    int      expanded;               /* for display: 1=children shown in flat view */
};

/* ── Global state ───────────────────────────────────────────────────── */

static struct ncdu_entry entries[NCDU_MAX_ENTRIES];
static int entry_count = 0;
static char scan_root[NCDU_MAX_PATH] = "/";
static uint64_t total_scan_size = 0;

/* ── Error tracking ─────────────────────────────────────────────────── */

static int scan_errors = 0;

/* ── Forward declarations ──────────────────────────────────────────── */

static int ncdu_scan(const char *path, int parent_idx, int depth);
static void ncdu_display(int top_idx, int cursor_pos, int scroll_offset);
static void __attribute__((unused)) ncdu_sort_children(int parent_idx);

/* ── Helper: ensure path ends with '/' for directory ops ──────────── */

static void ensure_trailing_slash(char *path, int max_len)
{
    int len = (int)strlen(path);
    if (len > 0 && len < max_len - 1 && path[len - 1] != '/') {
        path[len] = '/';
        path[len + 1] = '\0';
    }
}

/* ── Helper: create a full path by joining parent path + name ──────── */

static void build_path(const char *parent_path, const char *name,
                       char *out, int max_out)
{
    int plen = (int)strlen(parent_path);
    int nlen = (int)strlen(name);

    /* Copy parent first */
    if (plen >= max_out) plen = max_out - 1;
    memcpy(out, parent_path, plen);
    out[plen] = '\0';

    /* Ensure trailing slash */
    if (plen > 0 && out[plen - 1] != '/') {
        if (plen + 1 < max_out) {
            out[plen] = '/';
            out[plen + 1] = '\0';
            plen++;
        }
    }

    /* Append name */
    if (plen + nlen < max_out) {
        memcpy(out + plen, name, nlen);
        out[plen + nlen] = '\0';
    }
}

/* ── Add an entry to the tree (returns index or -1 if full) ───────── */

static int ncdu_add_entry(const char *name, const char *full_path,
                          uint64_t file_size, uint8_t type, int parent_idx, int depth)
{
    if (entry_count >= NCDU_MAX_ENTRIES)
        return -1;

    int idx = entry_count++;
    struct ncdu_entry *e = &entries[idx];

    /* Clear and initialize */
    memset(e, 0, sizeof(*e));

    strncpy(e->name, name, sizeof(e->name) - 1);
    e->name[sizeof(e->name) - 1] = '\0';

    strncpy(e->full_path, full_path, sizeof(e->full_path) - 1);
    e->full_path[sizeof(e->full_path) - 1] = '\0';

    e->size      = file_size;
    e->file_size = file_size;
    e->type      = type;
    e->children  = -1;
    e->nchildren = 0;
    e->depth     = depth;
    e->visited   = 1;
    e->expanded  = 0;

    /* Link to parent */
    if (parent_idx >= 0) {
        struct ncdu_entry *parent = &entries[parent_idx];
        if (parent->children < 0) {
            /* First child */
            parent->children = idx;
        }
        parent->nchildren++;
    }

    return idx;
}

/* ── Compare function for qsort (descending by size) ───────────────── */

static int ncdu_cmp_desc(const void *a, const void *b)
{
    const struct ncdu_entry *ea = &entries[*(const int *)a];
    const struct ncdu_entry *eb = &entries[*(const int *)b];
    if (ea->size < eb->size) return  1;
    if (ea->size > eb->size) return -1;
    return 0;
}

/* ── Sort children of a node by size (descending) ─────────────────── */

static void __attribute__((unused))
ncdu_sort_children(int parent_idx)
{
    if (parent_idx < 0 || parent_idx >= entry_count)
        return;

    struct ncdu_entry *parent = &entries[parent_idx];
    if (parent->nchildren <= 0 || parent->children < 0)
        return;

    /* Collect child indices into a temporary array */
    int *child_idx = (int *)libc_malloc((size_t)parent->nchildren * sizeof(int));
    if (!child_idx) return;

    int count = 0;
    for (int i = parent->children; i < entry_count && count < parent->nchildren; i++) {
        if (entries[i].depth == parent->depth + 1 && entries[i].visited) {
            /* This child belongs to us if its parent_idx matches */
            /* We verify by checking that the path starts with parent's path */
            if (strncmp(entries[i].full_path, parent->full_path,
                        strlen(parent->full_path)) == 0) {
                child_idx[count++] = i;
            }
        }
    }

    if (count > 1) {
        qsort(child_idx, (size_t)count, sizeof(int), ncdu_cmp_desc);
    }

    /* Re-link children in sorted order by updating the linked-list chain */
    /* Since entries are in a flat array, we just re-sort the child_idx array
     * and store them.  The display logic will iterate through siblings. */

    libc_free(child_idx);
}

/* ── Recursive scan: walk directory and collect stats ──────────────── */

static int ncdu_scan(const char *path, int parent_idx, int depth)
{
    char names[256][FS_MAX_NAME];
    int  count;
    char dir_path[NCDU_MAX_PATH];

    /* Clamp depth to prevent stack overflow */
    if (depth > 64)
        return -1;

    strncpy(dir_path, path, sizeof(dir_path) - 1);
    dir_path[sizeof(dir_path) - 1] = '\0';
    ensure_trailing_slash(dir_path, (int)sizeof(dir_path));

    /* Read directory entries */
    count = fs_list_names(dir_path, "", names, 256);
    if (count < 0) {
        scan_errors++;
        return -1;
    }

    /* Skip "." and ".." */
    int actual_count = 0;
    for (int i = 0; i < count; i++) {
        if (strcmp(names[i], ".") == 0 || strcmp(names[i], "..") == 0)
            continue;
        actual_count++;
    }

    /* Pre-scan to add all entries first */
    int first_child_idx = -1;

    for (int i = 0; i < count; i++) {
        if (strcmp(names[i], ".") == 0 || strcmp(names[i], "..") == 0)
            continue;

        char full_path[NCDU_MAX_PATH];
        build_path(dir_path, names[i], full_path, (int)sizeof(full_path));

        struct vfs_stat st;
        int st_ret = vfs_stat(full_path, &st);

        uint8_t type = FS_TYPE_FILE;
        uint64_t file_size = 0;

        if (st_ret == 0) {
            type = st.type;
            file_size = st.size;
        }

        int child_idx = ncdu_add_entry(names[i], full_path, file_size,
                                        type, parent_idx, depth + 1);
        if (child_idx < 0) {
            /* Tree is full — stop scanning */
            return 0;
        }

        if (first_child_idx < 0)
            first_child_idx = child_idx;
    }

    /* Now recursively scan directories */
    int child_base = first_child_idx;
    for (int i = (child_base >= 0 ? child_base : 0); i < entry_count; i++) {
        struct ncdu_entry *e = &entries[i];
        if (e->depth != depth + 1 || e->type != FS_TYPE_DIR)
            continue;

        /* Verify this child belongs to our parent */
        if (parent_idx >= 0) {
            int p_len = (int)strlen(entries[parent_idx].full_path);
            if (strncmp(e->full_path, entries[parent_idx].full_path, p_len) != 0)
                continue;
        }

        /* Recursively scan */
        ncdu_scan(e->full_path, i, depth + 1);
    }

    /* Compute cumulative size for this directory by summing children */
    if (parent_idx >= 0) {
        uint64_t total = 0;
        uint64_t child_file_sizes = 0;

        for (int i = 0; i < entry_count; i++) {
            if (entries[i].depth == depth + 1) {
                /* Check if it's our child by path prefix */
                int p_len = (int)strlen(entries[parent_idx].full_path);
                if (strncmp(entries[i].full_path,
                            entries[parent_idx].full_path, p_len) == 0) {
                    total += entries[i].size;
                    child_file_sizes += entries[i].file_size;
                }
            }
        }

        /* Add this directory's own file size (usually 0 for dirs) */
        entries[parent_idx].size = total + entries[parent_idx].file_size;
    }

    return 0;
}

/* ── Build a flattened display list from the tree ─────────────────── */

struct display_entry {
    int   entry_idx;         /* index into entries[] */
    int   depth;             /* display indentation level */
};

static struct display_entry display_list[NCDU_MAX_ENTRIES];
static int display_count = 0;

/* Recursively flatten the tree, respecting expanded state */
static void flatten_tree(int idx, int depth)
{
    if (idx < 0 || idx >= entry_count || display_count >= NCDU_MAX_ENTRIES)
        return;

    struct ncdu_entry *e = &entries[idx];

    /* Add this entry to the display list */
    display_list[display_count].entry_idx = idx;
    display_list[display_count].depth     = depth;
    display_count++;

    /* If this is a directory and expanded, add children */
    if (e->type == FS_TYPE_DIR && e->expanded) {
        /* Find children (entries with depth+1 that have the right prefix) */
        int parent_path_len = (int)strlen(e->full_path);
        for (int i = 0; i < entry_count; i++) {
            if (entries[i].depth == e->depth + 1 && entries[i].visited) {
                if (strncmp(entries[i].full_path, e->full_path,
                            parent_path_len) == 0) {
                    flatten_tree(i, depth + 1);
                }
            }
        }
    }
}

/* ── Sort children of a node using the display list ───────────────── */
/* Re-sort the children of node @idx in entries[] order */

static void sort_children_of(int idx)
{
    if (idx < 0 || idx >= entry_count) return;
    struct ncdu_entry *parent = &entries[idx];
    if (parent->nchildren <= 0) return;

    /* Collect child indices */
    int *child_idx = (int *)libc_malloc((size_t)parent->nchildren * sizeof(int));
    if (!child_idx) return;

    int p_len = (int)strlen(parent->full_path);
    int count = 0;
    for (int i = 0; i < entry_count && count < parent->nchildren; i++) {
        if (entries[i].depth == parent->depth + 1 && entries[i].visited) {
            if (strncmp(entries[i].full_path, parent->full_path, p_len) == 0) {
                child_idx[count++] = i;
            }
        }
    }

    if (count > 1) {
        qsort(child_idx, (size_t)count, sizeof(int), ncdu_cmp_desc);
    }

    /* Sort entries by reordering the child indices in entries[] — we can't
     * easily reorder the flat array, so we just re-sort the display list
     * when it's built.  The entries[] ordering doesn't matter for display. */

    libc_free(child_idx);
}

/* ── Display the ncdu screen ──────────────────────────────────────── */

static const char *format_size(uint64_t bytes)
{
    static char buf[32];
    if (bytes >= 1099511627776ULL) {
        snprintf(buf, sizeof(buf), "%5.1f TiB",
                 (double)bytes / 1099511627776.0);
    } else if (bytes >= 1073741824ULL) {
        snprintf(buf, sizeof(buf), "%5.1f GiB",
                 (double)bytes / 1073741824.0);
    } else if (bytes >= 1048576ULL) {
        snprintf(buf, sizeof(buf), "%5.1f MiB",
                 (double)bytes / 1048576.0);
    } else if (bytes >= 1024ULL) {
        snprintf(buf, sizeof(buf), "%5.1f KiB",
                 (double)bytes / 1024.0);
    } else {
        snprintf(buf, sizeof(buf), "%5llu  B",
                 (unsigned long long)bytes);
    }
    return buf;
}

/* Draw a horizontal bar representing the size proportion */
static void __attribute__((unused)) draw_bar(uint64_t size, uint64_t max_size, int width)
{
    if (max_size == 0 || size == 0) {
        for (int i = 0; i < width; i++)
            kprintf(" ");
        return;
    }

    int filled = (int)((uint64_t)size * (uint64_t)width / max_size);
    if (filled > width) filled = width;
    if (filled < 1 && size > 0) filled = 1;

    kprintf("[");
    vga_set_color(VGA_YELLOW, VGA_BLACK);
    for (int i = 0; i < filled; i++)
        kprintf("#");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
    for (int i = filled; i < width; i++)
        kprintf(" ");
    kprintf("]");
}

static void ncdu_display(int top_idx, int cursor_pos, int scroll_offset)
{
    /* Clear screen */
    vga_set_color(VGA_WHITE, VGA_BLACK);
    vga_set_cursor(0, 0);

    /* Build a flat display list from the root */
    display_count = 0;
    flatten_tree(top_idx, 0);

    /* Show header */
    vga_set_color(VGA_WHITE, VGA_BLUE);
    kprintf("┌─ ncdu ───────────────────────────────────────────────────────────┐\n");
    vga_set_color(VGA_CYAN, VGA_BLUE);
    kprintf("│  ");
    vga_set_color(VGA_WHITE, VGA_BLUE);
    kprintf("Disk Usage Analyzer — Scanning: %s", scan_root);
    vga_set_color(VGA_CYAN, VGA_BLUE);
    /* Pad to fill line */
    for (int i = (int)strlen(scan_root) + 30; i < 78; i++)
        kprintf(" ");
    kprintf("│\n");
    vga_set_color(VGA_WHITE, VGA_BLUE);
    kprintf("├──────────────────────────────────────────────────────────────────┤\n");
    vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);

    /* Find the largest size among displayed entries for bar scaling */
    uint64_t max_display_size = 0;
    for (int i = 0; i < display_count; i++) {
        int e_idx = display_list[i].entry_idx;
        if (e_idx >= 0 && e_idx < entry_count) {
            if (entries[e_idx].size > max_display_size)
                max_display_size = entries[e_idx].size;
        }
    }
    if (max_display_size == 0) max_display_size = 1;

    /* Adjust max for root which dominates */
    if (top_idx >= 0 && top_idx < entry_count) {
        uint64_t root_size = entries[top_idx].size;
        if (root_size > max_display_size)
            max_display_size = root_size;
    }
    if (max_display_size == 0) max_display_size = 1;

    /* Draw listing */
    int lines_drawn = 0;
    for (int i = scroll_offset; i < display_count && lines_drawn < NCDU_DISPLAY_LINES; i++)
    {
        int e_idx = display_list[i].entry_idx;
        if (e_idx < 0 || e_idx >= entry_count) continue;

        struct ncdu_entry *e = &entries[e_idx];
        int depth = display_list[i].depth;

        /* Highlight cursor row */
        if (i == cursor_pos) {
            vga_set_color(VGA_WHITE, VGA_DARK_GREY);
        } else {
            vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        }

        /* Indentation */
        for (int d = 0; d < depth; d++) {
            kprintf("  ");
        }

        /* Icon */
        if (e->type == FS_TYPE_DIR) {
            if (e->expanded)
                kprintf("▼ ");
            else
                kprintf("▶ ");
        }

        /* Percentage of total */
        uint64_t total = (top_idx >= 0 && top_idx < entry_count)
                         ? entries[top_idx].size : total_scan_size;
        if (total == 0) total = 1;
        int pct = (int)((uint64_t)e->size * 100ULL / total);
        if (pct > 99) pct = 99;

        /* Format size */
        const char *sizestr = format_size(e->size);

        /* Bar graph (compact) */
        kprintf("%s ", sizestr);
        vga_set_color(VGA_GREEN, VGA_BLACK);
        kprintf("%2d%% ", (int)pct);
        if (i == cursor_pos)
            vga_set_color(VGA_WHITE, VGA_DARK_GREY);
        else
            vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        kprintf("%s", e->name);

        /* Show directory marker */
        if (e->type == FS_TYPE_DIR) {
            vga_set_color(VGA_CYAN, VGA_BLACK);
            kprintf("/");
            vga_set_color(VGA_LIGHT_GREY, VGA_BLACK);
        }

        /* Clear rest of line */
        int used = (int)strlen(sizestr) + 3 + 3 + (int)strlen(e->name) + 1 + depth * 2;
        if (e->type == FS_TYPE_DIR) used++;
        while (used < 78) { kprintf(" "); used++; }

        lines_drawn++;
    }

    /* Fill remaining lines */
    while (lines_drawn < NCDU_DISPLAY_LINES) {
        for (int i = 0; i < 79; i++) kprintf(" ");
        kprintf("\n");
        lines_drawn++;
    }

    /* Footer */
    vga_set_color(VGA_WHITE, VGA_BLUE);
    kprintf("├──────────────────────────────────────────────────────────────────┤\n");
    vga_set_color(VGA_CYAN, VGA_BLUE);
    kprintf("│ ");
    vga_set_color(VGA_WHITE, VGA_BLUE);
    kprintf("↑↓ scroll   Enter open dir   d delete   q quit   %d/%d entries",
            display_count > 0 ? cursor_pos + 1 : 0, display_count);
    vga_set_color(VGA_CYAN, VGA_BLUE);
    for (int i = 56; i < 78; i++) kprintf(" ");
    kprintf("│\n");
    vga_set_color(VGA_WHITE, VGA_BLUE);
    kprintf("└──────────────────────────────────────────────────────────────────┘\n");
    vga_set_color(VGA_WHITE, VGA_BLACK);
}

/* ── Main interactive loop ─────────────────────────────────────────── */

static void ncdu_interactive(void)
{
    int cursor_pos = 0;       /* index into display_list */
    int scroll_offset = 0;
    int current_view_root = 0;  /* entries[] index of current view root */

    /* Start at the real root of the scanned tree */
    /* The root is the first entry (index 0) */
    current_view_root = 0;

    /* Initial sort: root's children */
    sort_children_of(current_view_root);

    for (;;) {
        ncdu_display(current_view_root, cursor_pos, scroll_offset);

        char ch = keyboard_getchar();

        switch (ch) {
        case 'q':
        case 'Q':
        case 0x1B:  /* Escape */
            return;

        case KEY_UP: {
            cursor_pos--;
            if (cursor_pos < 0) cursor_pos = 0;
            /* Scroll if cursor went above viewport */
            if (cursor_pos < scroll_offset)
                scroll_offset = cursor_pos;
            break;
        }

        case KEY_DOWN: {
            cursor_pos++;
            if (cursor_pos >= display_count)
                cursor_pos = display_count - 1;
            /* Scroll if cursor went below viewport */
            if (cursor_pos >= scroll_offset + NCDU_DISPLAY_LINES)
                scroll_offset = cursor_pos - NCDU_DISPLAY_LINES + 1;
            break;
        }

        case KEY_LEFT: {
            /* Go to parent directory if viewing a subdir */
            if (current_view_root >= 0 && current_view_root < entry_count) {
                int parent_depth = entries[current_view_root].depth - 1;
                if (parent_depth >= 0) {
                    /* Find parent by matching path prefix */
                    for (int i = 0; i < entry_count; i++) {
                        if (entries[i].depth == parent_depth && entries[i].visited) {
                            if (strncmp(entries[i].full_path,
                                        entries[current_view_root].full_path,
                                        strlen(entries[i].full_path)) == 0) {
                                current_view_root = i;
                                entries[i].expanded = 1;
                                cursor_pos = 0;
                                scroll_offset = 0;
                                break;
                            }
                        }
                    }
                }
            }
            break;
        }

        case KEY_RIGHT:
        case '\r':   /* Enter */
        case '\n': {
            /* Enter directory (expand/collapse or dive in) */
            if (cursor_pos < display_count) {
                int e_idx = display_list[cursor_pos].entry_idx;
                if (e_idx >= 0 && e_idx < entry_count &&
                    entries[e_idx].type == FS_TYPE_DIR) {
                    /* Toggle expanded state */
                    entries[e_idx].expanded = !entries[e_idx].expanded;
                    if (entries[e_idx].expanded) {
                        /* Sort children */
                        sort_children_of(e_idx);
                    }
                    cursor_pos = 0;
                    scroll_offset = 0;
                }
            }
            break;
        }

        case 'd':
        case 'D': {
            /* Delete the selected file */
            if (cursor_pos < display_count) {
                int e_idx = display_list[cursor_pos].entry_idx;
                if (e_idx >= 0 && e_idx < entry_count) {
                    struct ncdu_entry *e = &entries[e_idx];
                    if (e->type == FS_TYPE_FILE) {
                        /* Confirm deletion */
                        vga_set_color(VGA_WHITE, VGA_RED);
                        kprintf("\nDelete '%s'? (y/N): ", e->name);
                        char confirm = keyboard_getchar();
                        vga_set_color(VGA_WHITE, VGA_BLACK);
                        if (confirm == 'y' || confirm == 'Y') {
                            if (vfs_unlink(e->full_path) == 0) {
                                e->size = 0;
                                e->file_size = 0;
                                e->visited = 0; /* mark as deleted */
                            }
                        }
                    } else if (e->type == FS_TYPE_DIR) {
                        vga_set_color(VGA_YELLOW, VGA_BLACK);
                        kprintf("\nCannot delete directory '%s' with D — use rm instead\n",
                                e->name);
                        /* Pause briefly */
                        for (volatile int p = 0; p < 5000000; p++);
                    }
                }
            }
            break;
        }
        default: break;
        }
    }
}

/* ── Public entry point ─────────────────────────────────────────────── */

void cmd_ncdu(const char *args)
{
    /* Parse path argument */
    if (args && args[0]) {
        char buf[NCDU_MAX_PATH];
        strncpy(buf, args, sizeof(buf) - 1);
        buf[sizeof(buf) - 1] = '\0';

        /* Strip trailing spaces and newlines */
        int len = (int)strlen(buf);
        while (len > 0 && (buf[len-1] == ' ' || buf[len-1] == '\n'))
            buf[--len] = '\0';

        if (len > 0) {
            strncpy(scan_root, buf, sizeof(scan_root) - 1);
            scan_root[sizeof(scan_root) - 1] = '\0';
        }

        /* Ensure leading '/' */
        if (scan_root[0] != '/') {
            memmove(scan_root + 1, scan_root, strlen(scan_root) + 1);
            scan_root[0] = '/';
        }
    } else {
        strncpy(scan_root, "/", sizeof(scan_root) - 1);
    }

    /* Remove trailing '/' for display, except root */
    int rl = (int)strlen(scan_root);
    while (rl > 1 && scan_root[rl - 1] == '/')
        scan_root[--rl] = '\0';

    /* Initialize */
    memset(entries, 0, sizeof(entries));
    entry_count = 0;
    total_scan_size = 0;
    scan_errors = 0;

    /* Add root entry */
    {
        struct vfs_stat st_root;
        uint64_t root_size = 0;
        if (vfs_stat(scan_root, &st_root) == 0)
            root_size = st_root.size;

        ncdu_add_entry(scan_root, scan_root, root_size,
                        FS_TYPE_DIR, -1, 0);
    }

    /* Scan the filesystem */
    vga_set_color(VGA_WHITE, VGA_BLACK);
    kprintf("ncdu: Scanning %s ...\n", scan_root);

    ncdu_scan(scan_root, 0, 0);

    /* Compute root size from children */
    entries[0].size = 0;
    for (int i = 0; i < entry_count; i++) {
        entries[0].size += entries[i].size;
    }
    total_scan_size = entries[0].size;

    /* Sort root's children */
    sort_children_of(0);

    /* Show results */
    kprintf("ncdu: Scan complete — %d entries, %llu bytes total",
            entry_count, (unsigned long long)total_scan_size);
    if (scan_errors > 0) {
        kprintf(" (%d errors)", scan_errors);
    }
    kprintf("\n");
    kprintf("Press any key to start browsing...\n");
    keyboard_getchar();

    /* Enter interactive mode */
    ncdu_interactive();

    /* Clean exit */
    vga_set_color(VGA_WHITE, VGA_BLACK);
    kprintf("\nncdu: Done\n");
}

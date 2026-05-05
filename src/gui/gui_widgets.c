#include "gui_widgets.h"
#include "vga.h"
#include "string.h"
#include "heap.h"
#include "printf.h"

#define MAX_FILES 64
#define MAX_PATH_LEN 256

static void append_cstr(char *dst, uint32_t cap, const char *src) {
    uint32_t i = 0;
    while (i + 1 < cap && dst[i]) i++;
    if (i + 1 >= cap) return;
    uint32_t j = 0;
    while (i + 1 < cap && src[j]) {
        dst[i++] = src[j++];
    }
    dst[i] = '\0';
}

static void append_u32(char *dst, uint32_t cap, uint32_t v) {
    char tmp[16];
    uint32_t n = 0;
    if (v == 0) {
        append_cstr(dst, cap, "0");
        return;
    }
    while (v && n < (uint32_t)(sizeof(tmp) - 1)) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n > 0) {
        char one[2] = {tmp[--n], '\0'};
        append_cstr(dst, cap, one);
    }
}

/* File browser implementation */
struct gui_filebrowser {
    gui_widget_t widget;
    char current_path[MAX_PATH_LEN];
    char selected[MAX_PATH_LEN];
    int selected_index;
    struct {
        char name[64];
        int is_dir;
        uint32_t size;
    } files[MAX_FILES];
    int file_count;
    int scroll_offset;
    gui_filebrowser_on_select_fn on_select;
};

static void fb_refresh_list(gui_filebrowser_t *fb) {
    memset(fb->files, 0, sizeof(fb->files));
    fb->file_count = 0;
    
    /* List directory using VFS */
    int count = 0;
    /* Simplified: just add parent dir entry and a few dummy files */
    /* In production, would use fs_list() or vfs_readdir() */
    if (strcmp(fb->current_path, "/") != 0) {
        strcpy(fb->files[count].name, "..");
        fb->files[count].is_dir = 1;
        count++;
    }
    
    /* Add some example entries */
    if (count < MAX_FILES) {
        strcpy(fb->files[count].name, "home");
        fb->files[count].is_dir = 1;
        count++;
    }
    if (count < MAX_FILES) {
        strcpy(fb->files[count].name, "boot");
        fb->files[count].is_dir = 1;
        count++;
    }
    if (count < MAX_FILES) {
        strcpy(fb->files[count].name, "kernel.elf");
        fb->files[count].is_dir = 0;
        fb->files[count].size = 1024000;
        count++;
    }
    
    fb->file_count = count;
    fb->selected_index = 0;
    memset(fb->selected, 0, sizeof(fb->selected));
}

static void fb_draw(gui_widget_t *w) {
    gui_filebrowser_t *fb = (gui_filebrowser_t *)w->data;
    
    /* Draw background */
    gui_rect_t rect = {fb->widget.rect.x, fb->widget.rect.y, 
                       fb->widget.rect.w, fb->widget.rect.h};
    for (int32_t y = rect.y; y < (int32_t)(rect.y + rect.h); y++) {
        for (int32_t x = rect.x; x < (int32_t)(rect.x + rect.w); x++) {
            vga_put_pixel(x, y, GUI_WHITE);
        }
    }
    
    /* Draw border */
    gui_rect_t outline = {rect.x, rect.y, rect.w - 1, rect.h - 1};
    gui_window_draw_rect_outline(NULL, outline, GUI_DARK_GRAY, 1);
    
    /* Draw title (path) */
    gui_window_draw_text(NULL, rect.x + 4, rect.y + 4, 
                        fb->current_path, GUI_TEXT_FG, GUI_WHITE);
    
    /* Draw file list */
    int line_height = 16;
    int start_y = rect.y + 24;
    int visible_lines = (rect.h - 24) / line_height;
    
    for (int i = 0; i < visible_lines && i + fb->scroll_offset < fb->file_count; i++) {
        int idx = i + fb->scroll_offset;
        int32_t y = start_y + i * line_height;
        
        /* Highlight selected */
        gui_color_t bg = (idx == fb->selected_index) ? GUI_BUTTON_BG : GUI_WHITE;
        gui_color_t fg = (idx == fb->selected_index) ? GUI_BUTTON_FG : GUI_TEXT_FG;
        
        /* Draw file entry */
        char entry[128];
        entry[0] = '\0';
        if (fb->files[idx].is_dir) {
            append_cstr(entry, sizeof(entry), "[DIR] ");
            append_cstr(entry, sizeof(entry), fb->files[idx].name);
        } else {
            append_cstr(entry, sizeof(entry), fb->files[idx].name);
            append_cstr(entry, sizeof(entry), " (");
            append_u32(entry, sizeof(entry), fb->files[idx].size);
            append_cstr(entry, sizeof(entry), ")");
        }
        
        gui_window_draw_rect(NULL, (gui_rect_t){rect.x + 2, y, rect.w - 4, line_height}, bg);
        gui_window_draw_text(NULL, rect.x + 8, y + 2, entry, fg, bg);
    }
}

static void fb_event(gui_widget_t *w, gui_event_t *evt) {
    gui_filebrowser_t *fb = (gui_filebrowser_t *)w->data;
    
    if (evt->type == GUI_EVENT_MOUSE_DOWN && evt->button == 1) {
        int line_height = 16;
        int start_y = fb->widget.rect.y + 24;
        int relative_y = evt->y - start_y;
        int line = relative_y / line_height;
        
        if (line >= 0 && line < (int)(fb->widget.rect.h - 24) / line_height) {
            int idx = line + fb->scroll_offset;
            if (idx < fb->file_count) {
                fb->selected_index = idx;
                strcpy(fb->selected, fb->files[idx].name);
                
                if (fb->on_select) {
                    fb->on_select(fb, fb->selected);
                }
            }
        }
    }
}

static void fb_destroy(gui_widget_t *w) {
    gui_filebrowser_t *fb = (gui_filebrowser_t *)w->data;
    if (fb) kfree(fb);
}

gui_filebrowser_t* gui_filebrowser_create(gui_rect_t rect, const char *path) {
    gui_filebrowser_t *fb = kmalloc(sizeof(gui_filebrowser_t));
    if (!fb) return NULL;
    
    memset(fb, 0, sizeof(gui_filebrowser_t));
    fb->widget.rect = rect;
    fb->widget.bg = GUI_WHITE;
    fb->widget.fg = GUI_TEXT_FG;
    fb->widget.visible = 1;
    fb->widget.data = fb;
    fb->widget.draw = fb_draw;
    fb->widget.on_event = fb_event;
    fb->widget.destroy = fb_destroy;
    
    if (path) {
        strncpy(fb->current_path, path, sizeof(fb->current_path) - 1);
    } else {
        strcpy(fb->current_path, "/");
    }
    
    fb_refresh_list(fb);
    return fb;
}

void gui_filebrowser_destroy(gui_filebrowser_t *fb) {
    if (fb) kfree(fb);
}

gui_widget_t* gui_filebrowser_get_widget(gui_filebrowser_t *fb) {
    return fb ? &fb->widget : NULL;
}

void gui_filebrowser_set_path(gui_filebrowser_t *fb, const char *path) {
    if (!fb || !path) return;
    strncpy(fb->current_path, path, sizeof(fb->current_path) - 1);
    fb_refresh_list(fb);
}

const char* gui_filebrowser_get_path(gui_filebrowser_t *fb) {
    return fb ? fb->current_path : NULL;
}

const char* gui_filebrowser_get_selected(gui_filebrowser_t *fb) {
    return fb ? fb->selected : NULL;
}

void gui_filebrowser_set_on_select(gui_filebrowser_t *fb, gui_filebrowser_on_select_fn fn) {
    if (fb) fb->on_select = fn;
}

/* Taskbar implementation */
struct gui_taskbar {
    gui_widget_t widget;
    struct {
        gui_widget_t *btn;
        char label[64];
    } buttons[16];
    int button_count;
};

static void tb_draw(gui_widget_t *w) {
    gui_taskbar_t *tb = (gui_taskbar_t *)w->data;
    
    /* Draw background */
    gui_rect_t rect = {tb->widget.rect.x, tb->widget.rect.y, 
                       tb->widget.rect.w, tb->widget.rect.h};
    for (int32_t y = rect.y; y < (int32_t)(rect.y + rect.h); y++) {
        for (int32_t x = rect.x; x < (int32_t)(rect.x + rect.w); x++) {
            vga_put_pixel(x, y, GUI_DARK_GRAY);
        }
    }
    
    /* Draw buttons */
    int x_pos = rect.x + 4;
    for (int i = 0; i < tb->button_count; i++) {
        if (tb->buttons[i].btn) {
            tb->buttons[i].btn->rect.x = x_pos;
            tb->buttons[i].btn->rect.y = rect.y + 2;
            tb->buttons[i].btn->rect.h = rect.h - 4;
            gui_widget_draw(tb->buttons[i].btn);
            x_pos += tb->buttons[i].btn->rect.w + 4;
        }
    }
}

static void tb_event(gui_widget_t *w, gui_event_t *evt) {
    gui_taskbar_t *tb = (gui_taskbar_t *)w->data;
    
    for (int i = 0; i < tb->button_count; i++) {
        if (tb->buttons[i].btn && gui_widget_contains_point(tb->buttons[i].btn, evt->x, evt->y)) {
            gui_widget_on_event(tb->buttons[i].btn, evt);
        }
    }
}

static void tb_destroy(gui_widget_t *w) {
    gui_taskbar_t *tb = (gui_taskbar_t *)w->data;
    if (!tb) return;
    for (int i = 0; i < tb->button_count; i++) {
        if (tb->buttons[i].btn) gui_widget_destroy(tb->buttons[i].btn);
    }
    kfree(tb);
}

gui_taskbar_t* gui_taskbar_create(gui_rect_t rect) {
    gui_taskbar_t *tb = kmalloc(sizeof(gui_taskbar_t));
    if (!tb) return NULL;
    
    memset(tb, 0, sizeof(gui_taskbar_t));
    tb->widget.rect = rect;
    tb->widget.bg = GUI_DARK_GRAY;
    tb->widget.fg = GUI_WHITE;
    tb->widget.visible = 1;
    tb->widget.data = tb;
    tb->widget.draw = tb_draw;
    tb->widget.on_event = tb_event;
    tb->widget.destroy = tb_destroy;
    
    return tb;
}

void gui_taskbar_destroy(gui_taskbar_t *tb) {
    if (tb) {
        tb_destroy(&tb->widget);
    }
}

gui_widget_t* gui_taskbar_get_widget(gui_taskbar_t *tb) {
    return tb ? &tb->widget : NULL;
}

void gui_taskbar_add_button(gui_taskbar_t *tb, const char *label, 
                            void (*on_click)(gui_widget_t *)) {
    if (!tb || tb->button_count >= 16) return;
    
    gui_rect_t btn_rect = {0, 0, 80, 20};
    gui_widget_t *btn = gui_button_create(btn_rect, label);
    if (btn) {
        gui_button_set_on_click(btn, on_click);
        tb->buttons[tb->button_count].btn = btn;
        strcpy(tb->buttons[tb->button_count].label, label);
        tb->button_count++;
    }
}

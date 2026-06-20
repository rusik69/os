#ifndef GUI_FILEBROWSER_H
#define GUI_FILEBROWSER_H

#include "gui.h"

/* File browser widget */
typedef struct gui_filebrowser gui_filebrowser_t;

gui_filebrowser_t* gui_filebrowser_create(gui_rect_t rect, const char *path);
void gui_filebrowser_destroy(gui_filebrowser_t *fb);
gui_widget_t* gui_filebrowser_get_widget(gui_filebrowser_t *fb);

void gui_filebrowser_set_path(gui_filebrowser_t *fb, const char *path);
const char* gui_filebrowser_get_path(gui_filebrowser_t *fb);
const char* gui_filebrowser_get_selected(gui_filebrowser_t *fb);

typedef void (*gui_filebrowser_on_select_fn)(gui_filebrowser_t *fb, const char *path);
void gui_filebrowser_set_on_select(gui_filebrowser_t *fb, gui_filebrowser_on_select_fn fn);

/* Taskbar widget */
typedef struct gui_taskbar gui_taskbar_t;

gui_taskbar_t* gui_taskbar_create(gui_rect_t rect);
void gui_taskbar_destroy(gui_taskbar_t *tb);
gui_widget_t* gui_taskbar_get_widget(gui_taskbar_t *tb);

void gui_taskbar_add_button(gui_taskbar_t *tb, const char *label, 
                            void (*on_click)(gui_widget_t *));

#endif /* GUI_FILEBROWSER_H */

// KlipperScreen (Sovol SV08 fork) look and navigation, rebuilt in LVGL.
// Layout follows panels/base_panel.py: left action bar (back, home, [printer], [shortcut], e-stop),
// title bar (heater temperatures | title | clock) and a content area that panels fill.
#pragma once
#include "lvgl.h"

// z-bolt theme (styles/z-bolt/style.css)
#define KS_BG      lv_color_hex(0x13181C)
#define KS_ACTIVE  lv_color_hex(0x324A5E)
#define KS_ERROR   lv_color_hex(0x981E1F)
#define KS_WARNING lv_color_hex(0xF9A825)
#define KS_ECHO    lv_color_hex(0x007D70)
#define KS_BLUE    lv_color_hex(0x3584E4)   // switch / progress accent
lv_color_t ks_color(int n);                 // color1..color4 (1-based, wraps): orange, magenta, teal, lime

#define KS_ACTION_W 84
#define KS_TITLE_H  64

typedef void (*ks_panel_init)(lv_obj_t* content);

void ks_ui_start();                                   // build the screen and show the main menu
void ks_open(const char* title, ks_panel_init init);  // push a panel
void ks_back();                                       // pop one level (no-op at the main menu)
void ks_home();                                       // back to the main menu
void ks_set_title(const char* t);
lv_obj_t* ks_content();                               // current content container

// widgets
lv_obj_t* ks_img(lv_obj_t* parent, const char* icon, int px, lv_color_t color);          // KlipperScreen icon by name
lv_obj_t* ks_button(lv_obj_t* parent, const char* icon, const char* label, int color,    // KS menu button: icon over label,
                    lv_event_cb_t cb, void* user);                                       // coloured bottom edge (color 0 = none)
lv_obj_t* ks_label(lv_obj_t* parent, const char* text, const lv_font_t* font);
void ks_toast(const char* text);                      // transient message

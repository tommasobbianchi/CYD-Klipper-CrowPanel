// Shared widgets for the KlipperScreen panels (Sovol z-bolt look): scrolling list, frame-item rows, switch, scale,
// segmented toggle buttons (".horizontal_togglebuttons"), on-screen keyboard, state subscription.
#pragma once
#include "lvgl.h"
#include "ks_ui.h"

// ---- lists and rows ---------------------------------------------------------------------------------------
lv_obj_t* ks_list(lv_obj_t* parent);                                       // full-size vertically scrolling column
lv_obj_t* ks_row(lv_obj_t* list, const char* name, const char* sub = nullptr);   // .frame-item: name (+ grey sub text) on the left
lv_obj_t* ks_row_right(lv_obj_t* row, int width);                          // right-aligned flex row for the row's controls
lv_obj_t* ks_card(lv_obj_t* parent, int w, int h);                         // plain transparent container, no scroll, no border

// ---- controls ---------------------------------------------------------------------------------------------
lv_obj_t* ks_switch(lv_obj_t* parent, bool on, lv_event_cb_t on_changed, void* user);   // read with lv_obj_has_state(CHECKED)
// Scale like Gtk.Scale: blue progress, white knob. on_changed fires while dragging; on_released once at the end.
lv_obj_t* ks_slider(lv_obj_t* parent, int min, int max, int value, int width,
                    lv_event_cb_t on_released, void* user);                // read with lv_slider_get_value(target)
typedef void (*ks_toggle_cb)(int index, void* user);
// Segmented control. Returns the container; ks_toggles_select() changes the highlight without firing the callback.
lv_obj_t* ks_toggles(lv_obj_t* parent, const char* const* labels, int n, int selected, ks_toggle_cb cb, void* user);
void ks_toggles_select(lv_obj_t* toggles, int index);

// ---- live updates -----------------------------------------------------------------------------------------
// Call cb (LV_EVENT_MSG_RECEIVED) on obj whenever the printer status document changed (about once per second).
void ks_on_state(lv_obj_t* obj, lv_event_cb_t cb);

// ---- text entry -------------------------------------------------------------------------------------------
lv_obj_t* ks_textarea(lv_obj_t* parent, const char* placeholder, int width, bool password = false);   // tap opens ks_keyboard
void ks_keyboard_show(lv_obj_t* textarea);          // bottom keyboard bound to the textarea, closes on OK/cancel
void ks_keyboard_hide();

// ---- formatting ---------------------------------------------------------------------------------------------
void ks_fmt_duration(float seconds, char* out, size_t n);   // "1h 23m", "4m 05s", "12s"

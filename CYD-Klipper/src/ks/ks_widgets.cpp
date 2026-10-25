#include <Arduino.h>
#include "ks_widgets.h"
#include "ks_mr.h"

// ---- lists and rows -----------------------------------------------------------------------------------------
lv_obj_t* ks_list(lv_obj_t* parent) {
    lv_obj_t* l = lv_obj_create(parent);
    lv_obj_set_size(l, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(l, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(l, 0, 0);
    lv_obj_set_style_pad_all(l, 4, 0);
    lv_obj_set_style_pad_row(l, 4, 0);
    lv_obj_set_flex_flow(l, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(l, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(l, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(l, LV_SCROLLBAR_MODE_AUTO);
    return l;
}

lv_obj_t* ks_card(lv_obj_t* parent, int w, int h) {
    lv_obj_t* c = lv_obj_create(parent);
    lv_obj_set_size(c, w, h);
    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_radius(c, 0, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

lv_obj_t* ks_row(lv_obj_t* list, const char* name, const char* sub) {      // .frame-item
    lv_obj_t* r = lv_obj_create(list);
    lv_obj_set_size(r, lv_pct(100), sub ? 104 : 72);
    lv_obj_set_style_bg_color(r, KS_BG, 0);
    lv_obj_set_style_radius(r, 0, 0);
    lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(r, 1, 0);
    lv_obj_set_style_border_color(r, KS_ACTIVE, 0);
    lv_obj_set_style_pad_hor(r, 10, 0);
    lv_obj_set_style_pad_ver(r, 4, 0);
    lv_obj_clear_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* t = ks_card(r, 380, lv_pct(100));
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(t, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t* n = ks_label(t, name, &ks_font_28);
    lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);
    lv_obj_set_width(n, 380);
    if (sub) {
        lv_obj_t* s = ks_label(t, sub, &ks_font_18);
        lv_obj_set_style_text_color(s, lv_color_hex(0x9AA5B1), 0);
        lv_label_set_long_mode(s, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(s, 380);
    }
    return r;
}

lv_obj_t* ks_row_right(lv_obj_t* row, int width) {
    lv_obj_t* b = ks_card(row, width, lv_pct(100));
    lv_obj_align(b, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(b, 6, 0);
    return b;
}

// ---- controls ---------------------------------------------------------------------------------------------------
lv_obj_t* ks_switch(lv_obj_t* parent, bool on, lv_event_cb_t cb, void* user) {
    lv_obj_t* sw = lv_switch_create(parent);
    lv_obj_set_size(sw, 100, 52);
    lv_obj_set_style_bg_color(sw, KS_ACTIVE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, KS_BLUE, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(sw, lv_color_white(), LV_PART_KNOB);
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    if (cb) lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, user);
    return sw;
}

lv_obj_t* ks_slider(lv_obj_t* parent, int min, int max, int value, int width, lv_event_cb_t on_released, void* user) {
    lv_obj_t* s = lv_slider_create(parent);
    lv_slider_set_range(s, min, max);
    lv_slider_set_value(s, value, LV_ANIM_OFF);
    lv_obj_set_size(s, width, 30);
    lv_obj_set_style_bg_color(s, KS_ACTIVE, LV_PART_MAIN);            // trough
    lv_obj_set_style_bg_color(s, KS_BLUE, LV_PART_INDICATOR);         // progress
    lv_obj_set_style_bg_color(s, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(s, 10, LV_PART_KNOB);                    // big touch target
    if (on_released) lv_obj_add_event_cb(s, on_released, LV_EVENT_RELEASED, user);
    return s;
}

struct Toggles { ks_toggle_cb cb; void* user; int n; int sel; lv_obj_t* btns[8]; };

static void toggles_paint(Toggles* t) {
    for (int i = 0; i < t->n; i++) {
        bool on = i == t->sel;                                         // .horizontal_togglebuttons_active
        lv_obj_set_style_bg_color(t->btns[i], on ? KS_ACTIVE : KS_BG, 0);
        lv_obj_set_style_border_side(t->btns[i], LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(t->btns[i], on ? 5 : 0, 0);
        lv_obj_set_style_border_color(t->btns[i], ks_color(1), 0);
    }
}
static void toggles_click(lv_event_t* e) {
    Toggles* t = (Toggles*)lv_obj_get_user_data(lv_obj_get_parent(lv_event_get_target(e)));
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    t->sel = idx;
    toggles_paint(t);
    if (t->cb) t->cb(idx, t->user);
}
static void toggles_del(lv_event_t* e) { delete (Toggles*)lv_event_get_user_data(e); }

lv_obj_t* ks_toggles(lv_obj_t* parent, const char* const* labels, int n, int selected, ks_toggle_cb cb, void* user) {
    if (n > 8) n = 8;
    Toggles* t = new Toggles{cb, user, n, selected, {}};
    lv_obj_t* box = ks_card(parent, lv_pct(100), 64);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(box, 2, 0);
    lv_obj_set_user_data(box, t);
    lv_obj_add_event_cb(box, toggles_del, LV_EVENT_DELETE, t);
    for (int i = 0; i < n; i++) {
        lv_obj_t* b = ks_button(box, nullptr, nullptr, 0, toggles_click, (void*)(intptr_t)i);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_height(b, lv_pct(100));
        ks_label(b, labels[i], &ks_font_22);
        t->btns[i] = b;
    }
    toggles_paint(t);
    return box;
}

void ks_toggles_select(lv_obj_t* box, int index) {
    Toggles* t = (Toggles*)lv_obj_get_user_data(box);
    if (!t || index < 0 || index >= t->n) return;
    t->sel = index;
    toggles_paint(t);
}

// ---- live updates ------------------------------------------------------------------------------------------------
void ks_on_state(lv_obj_t* obj, lv_event_cb_t cb) {
    lv_obj_add_event_cb(obj, cb, LV_EVENT_MSG_RECEIVED, nullptr);
    lv_msg_subscribe_obj(KS_MSG_STATE, obj, nullptr);
}

// ---- text entry ---------------------------------------------------------------------------------------------------
static lv_obj_t* g_kb = nullptr;

void ks_keyboard_hide() {
    if (g_kb) { lv_obj_del(g_kb); g_kb = nullptr; }
}
static void kb_event(lv_event_t* e) {
    lv_event_code_t c = lv_event_get_code(e);
    if (c == LV_EVENT_READY || c == LV_EVENT_CANCEL) ks_keyboard_hide();
}
void ks_keyboard_show(lv_obj_t* ta) {
    ks_keyboard_hide();
    g_kb = lv_keyboard_create(lv_layer_top());
    lv_obj_set_size(g_kb, 800, 250);
    lv_obj_align(g_kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_text_font(g_kb, &ks_font_28, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(g_kb, KS_BG, 0);
    lv_obj_set_style_bg_color(g_kb, KS_ACTIVE, LV_PART_ITEMS);
    lv_obj_set_style_text_color(g_kb, lv_color_white(), LV_PART_ITEMS);
    lv_keyboard_set_textarea(g_kb, ta);
    lv_obj_add_event_cb(g_kb, kb_event, LV_EVENT_ALL, nullptr);
}
static void ta_click(lv_event_t* e) { ks_keyboard_show(lv_event_get_target(e)); }
static void ta_gone(lv_event_t*) { ks_keyboard_hide(); }

lv_obj_t* ks_textarea(lv_obj_t* parent, const char* placeholder, int width, bool password) {
    lv_obj_t* ta = lv_textarea_create(parent);
    lv_obj_set_width(ta, width);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, placeholder);
    lv_textarea_set_password_mode(ta, password);
    lv_obj_set_style_bg_color(ta, KS_ACTIVE, 0);                       // entry { background @active }
    lv_obj_set_style_text_color(ta, lv_color_white(), 0);
    lv_obj_set_style_text_font(ta, &ks_font_22, 0);
    lv_obj_set_style_border_width(ta, 0, 0);
    lv_obj_set_style_radius(ta, 0, 0);
    lv_obj_add_event_cb(ta, ta_click, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(ta, ta_gone, LV_EVENT_DELETE, nullptr);
    return ta;
}

// ---- formatting ---------------------------------------------------------------------------------------------------
void ks_fmt_duration(float seconds, char* out, size_t n) {
    int s = seconds < 0 ? 0 : (int)seconds;
    if (s >= 3600) snprintf(out, n, "%dh %02dm", s / 3600, (s % 3600) / 60);
    else if (s >= 60) snprintf(out, n, "%dm %02ds", s / 60, s % 60);
    else snprintf(out, n, "%ds", s);
}

#include "ks_ui.h"
#include "ks_mr.h"
#include "icons.h"
#include "ks_cfg.h"
#include "ks_panels.h"
#include <time.h>

void ks_menu_open_main();   // ks_menu.cpp: fills the content area with the main menu

lv_color_t ks_color(int n) {
    static const uint32_t c[4] = {0xED6500, 0xB10080, 0x009384, 0xA7E100};   // color1..color4
    return lv_color_hex(c[((n < 1 ? 1 : n) - 1) % 4]);
}

// ---- styles -----------------------------------------------------------------------------------
static lv_style_t st_btn, st_btn_pr, st_plain, st_btn_dis;
static bool g_styles = false;
static void styles_init() {
    if (g_styles) return;
    g_styles = true;
    lv_style_init(&st_plain);
    lv_style_set_bg_color(&st_plain, KS_BG);
    lv_style_set_bg_opa(&st_plain, LV_OPA_COVER);
    lv_style_set_border_width(&st_plain, 0);
    lv_style_set_radius(&st_plain, 0);
    lv_style_set_pad_all(&st_plain, 0);
    lv_style_set_text_color(&st_plain, lv_color_white());

    lv_style_init(&st_btn);                     // button { background @bg; border 0; radius 0 }
    lv_style_set_bg_color(&st_btn, KS_BG);
    lv_style_set_bg_opa(&st_btn, LV_OPA_COVER);
    lv_style_set_radius(&st_btn, 0);
    lv_style_set_border_width(&st_btn, 0);
    lv_style_set_shadow_width(&st_btn, 0);
    lv_style_set_outline_width(&st_btn, 0);
    lv_style_set_pad_all(&st_btn, 4);
    lv_style_set_text_color(&st_btn, lv_color_white());

    lv_style_init(&st_btn_dis);                 // button:disabled keeps the flat background, greyed content
    lv_style_set_bg_color(&st_btn_dis, KS_BG);
    lv_style_set_img_opa(&st_btn_dis, LV_OPA_30);
    lv_style_set_text_opa(&st_btn_dis, LV_OPA_40);

    lv_style_init(&st_btn_pr);                  // button:active { background @active }
    lv_style_set_bg_color(&st_btn_pr, KS_ACTIVE);
}

// ---- widgets -----------------------------------------------------------------------------------
lv_obj_t* ks_img(lv_obj_t* parent, const char* icon, int px, lv_color_t color) {
    lv_obj_t* im = lv_img_create(parent);
    const lv_img_dsc_t* d = ks_icon(icon, px);   // pre-rendered at the exact size, no runtime zoom
    if (d) lv_img_set_src(im, d);
    lv_obj_set_style_img_recolor(im, color, 0);
    lv_obj_set_style_img_recolor_opa(im, LV_OPA_COVER, 0);
    lv_obj_clear_flag(im, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return im;
}

lv_obj_t* ks_label(lv_obj_t* parent, const char* text, const lv_font_t* font) {
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font ? font : &ks_font_22, 0);
    lv_obj_set_style_text_color(l, lv_color_white(), 0);
    return l;
}

lv_obj_t* ks_button(lv_obj_t* parent, const char* icon, const char* label, int color, lv_event_cb_t cb, void* user) {
    return ks_button_sized(parent, icon, label ? 72 : 56, label, color, cb, user);
}

lv_obj_t* ks_button_sized(lv_obj_t* parent, const char* icon, int icon_px, const char* label, int color, lv_event_cb_t cb, void* user) {
    styles_init();
    lv_obj_t* b = lv_btn_create(parent);
    lv_obj_add_style(b, &st_btn, 0);
    lv_obj_add_style(b, &st_btn_pr, LV_STATE_PRESSED);
    lv_obj_add_style(b, &st_btn_dis, LV_STATE_DISABLED);
    if (color) {                                 // button.colorN { border-bottom: .4em solid colorN }
        lv_obj_set_style_border_side(b, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(b, 6, 0);
        lv_obj_set_style_border_color(b, ks_color(color), 0);
    }
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(b, 4, 0);
    if (icon) ks_img(b, icon, icon_px, lv_color_white());
    if (label) {
        lv_obj_t* l = ks_label(b, label, &ks_font_22);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);     // long names wrap onto a second line instead of being cut
        lv_obj_set_width(l, lv_pct(100));
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    }
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    return b;
}

void ks_toast(const char* text) {
    lv_obj_t* t = lv_obj_create(lv_layer_top());
    lv_obj_add_style(t, &st_btn, 0);
    lv_obj_set_style_bg_color(t, KS_ACTIVE, 0);
    lv_obj_set_style_pad_all(t, 12, 0);
    lv_obj_set_size(t, 520, LV_SIZE_CONTENT);
    lv_obj_align(t, LV_ALIGN_BOTTOM_MID, 0, -20);
    ks_label(t, text, &ks_font_22);
    lv_obj_del_delayed(t, 2500);
}

// ---- screen: action bar, title bar, content ---------------------------------------------------
static lv_obj_t *g_action, *g_title_bar, *g_content, *g_title_lbl, *g_time_lbl, *g_btn_back;
static lv_obj_t *g_ext_lbl, *g_bed_lbl;

struct Nav { const char* title; ks_panel_init init; };
static Nav g_stack[12];
static int g_depth = 0;     // number of pushed panels; 0 = main menu (or the splash screen while g_splash)
static bool g_splash = true;      // Klipper/Moonraker not usable -> panels/splash_screen.py
static bool g_was_conn = false;

lv_obj_t* ks_content() { return g_content; }
void ks_set_title(const char* t) { if (g_title_lbl) lv_label_set_text(g_title_lbl, t); }

static void show_current() {
    lv_obj_clean(g_content);
    if (g_depth == 0 && g_splash) {
        ks_set_title("Klipper");
        ks_panel_splash(g_content);
    } else if (g_depth == 0) {
        ks_set_title("Main Menu");
        ks_menu_open_main();
    } else {
        Nav& n = g_stack[g_depth - 1];
        ks_set_title(n.title);
        n.init(g_content);
    }
    if (g_btn_back) {
        bool root = g_depth == 0;
        if (root) lv_obj_clear_flag(g_btn_back, LV_OBJ_FLAG_CLICKABLE); else lv_obj_add_flag(g_btn_back, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_img_opa(lv_obj_get_child(g_btn_back, 0), root ? LV_OPA_30 : LV_OPA_COVER, 0);
    }
}
void ks_open(const char* title, ks_panel_init init) {
    if (g_depth < 12) g_stack[g_depth++] = {title, init};
    show_current();
}
void ks_back() { if (g_depth > 0) { g_depth--; show_current(); } }
void ks_home() { g_depth = 0; show_current(); }

static void on_back(lv_event_t*) { ks_back(); }
static void on_home(lv_event_t*) { ks_home(); }
static void do_estop(void*) { ks_post("/printer/emergency_stop", "{}"); ks_toast("Emergency stop sent"); }
static void on_estop(lv_event_t*) {   // screen_panel.emergency_stop: confirm only if the 'confirm_estop' option is on
    if (ks_cfg_bool("confirm_estop", false)) ks_confirm("Are you sure you want to run Emergency Stop?", "", "Emergency Stop", do_estop, nullptr);
    else do_estop(nullptr);
}
static void on_shortcut(lv_event_t*) { void ks_open_macros(); ks_open_macros(); }

static void fmt_temp(char* out, size_t n, const char* obj) {
    float t = ks_f(obj, "temperature", -1), tg = ks_f(obj, "target", 0);
    if (t < 0) snprintf(out, n, "--");
    else if (tg > 0) snprintf(out, n, "%.0f/%.0f°", t, tg);
    else snprintf(out, n, "%.0f°", t);
}
static void on_state(lv_event_t*) {
    bool conn = ks_connected();
    if (conn != g_was_conn) { g_was_conn = conn; g_splash = !conn; g_depth = 0; show_current(); }
    char buf[24], ext[40] = "extruder";
    ks_str("toolhead", "extruder", ext, sizeof ext);
    fmt_temp(buf, sizeof buf, ext);       lv_label_set_text(g_ext_lbl, buf);
    fmt_temp(buf, sizeof buf, "heater_bed"); lv_label_set_text(g_bed_lbl, buf);
    time_t now = time(nullptr);
    struct tm tm; localtime_r(&now, &tm);
    if (tm.tm_year < 120) lv_label_set_text(g_time_lbl, "--:--");
    else { strftime(buf, sizeof buf, "%H:%M", &tm); lv_label_set_text(g_time_lbl, buf); }
}

static lv_obj_t* action_btn(const char* icon, lv_event_cb_t cb, lv_color_t col) {
    lv_obj_t* b = ks_button(g_action, nullptr, nullptr, 0, cb, nullptr);
    lv_obj_set_width(b, lv_pct(100));
    lv_obj_set_flex_grow(b, 1);
    ks_img(b, icon, 56, col);
    return b;
}

void ks_ui_start() {
    styles_init();
    configTzTime("CET-1CEST,M3.5.0,M10.5.0/3", "pool.ntp.org", "time.cloudflare.com");

    lv_obj_t* scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_add_style(scr, &st_plain, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    g_action = lv_obj_create(scr);                         // .action_bar
    lv_obj_add_style(g_action, &st_plain, 0);
    lv_obj_set_size(g_action, KS_ACTION_W, 480);
    lv_obj_set_pos(g_action, 0, 0);
    lv_obj_set_flex_flow(g_action, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(g_action, 4, 0);
    lv_obj_set_style_pad_all(g_action, 2, 0);
    lv_obj_clear_flag(g_action, LV_OBJ_FLAG_SCROLLABLE);
    g_btn_back = action_btn("back", on_back, lv_color_white());
    action_btn("main", on_home, lv_color_white());
    action_btn("custom-script", on_shortcut, lv_color_white());   // shortcut -> gcode_macros
    lv_obj_t* es = action_btn("emergency", on_estop, lv_color_hex(0xFF4040));
    (void)es;

    g_title_bar = lv_obj_create(scr);                      // .title_bar
    lv_obj_add_style(g_title_bar, &st_plain, 0);
    lv_obj_set_size(g_title_bar, 800 - KS_ACTION_W, KS_TITLE_H);
    lv_obj_set_pos(g_title_bar, KS_ACTION_W, 0);
    lv_obj_clear_flag(g_title_bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* temps = lv_obj_create(g_title_bar);
    lv_obj_add_style(temps, &st_plain, 0);
    lv_obj_set_size(temps, 330, KS_TITLE_H);
    lv_obj_align(temps, LV_ALIGN_LEFT_MID, 6, 0);
    lv_obj_set_flex_flow(temps, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(temps, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(temps, 4, 0);
    lv_obj_clear_flag(temps, LV_OBJ_FLAG_SCROLLABLE);
    ks_img(temps, "extruder", 40, lv_color_white());
    g_ext_lbl = ks_label(temps, "--", &ks_font_22);
    lv_obj_set_style_pad_left(g_ext_lbl, 0, 0);
    lv_obj_t* sp = lv_obj_create(temps); lv_obj_add_style(sp, &st_plain, 0); lv_obj_set_size(sp, 8, 1);
    ks_img(temps, "bed", 40, lv_color_white());
    g_bed_lbl = ks_label(temps, "--", &ks_font_22);

    g_title_lbl = ks_label(g_title_bar, "Main Menu", &ks_font_28);
    lv_obj_align(g_title_lbl, LV_ALIGN_CENTER, 110, 0);
    g_time_lbl = ks_label(g_title_bar, "--:--", &ks_font_22);
    lv_obj_align(g_time_lbl, LV_ALIGN_RIGHT_MID, -12, 0);

    g_content = lv_obj_create(scr);                        // .content
    lv_obj_add_style(g_content, &st_plain, 0);
    lv_obj_set_size(g_content, 800 - KS_ACTION_W, 480 - KS_TITLE_H);
    lv_obj_set_pos(g_content, KS_ACTION_W, KS_TITLE_H);
    lv_obj_clear_flag(g_content, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_add_event_cb(g_title_bar, on_state, LV_EVENT_MSG_RECEIVED, nullptr);
    lv_msg_subscribe_obj(KS_MSG_STATE, g_title_bar, nullptr);

    g_depth = 0;
    show_current();
}

// panels/splash_screen.py: shown whenever Klipper/Moonraker is not usable (disconnected, startup, shutdown, error).
#include "ks_ui.h"
#include "ks_mr.h"
#include "ks_panels.h"

void ks_menu_open_splash();   // ks_menu.cpp: Power / Network / System / KlipperScreen

static lv_obj_t* g_text;

static void splash_text(char* out, size_t n) {
    const char* st = ks_klippy_state();
    if (!strcmp(st, "disconnected")) { snprintf(out, n, "Connecting to Moonraker..."); return; }
    if (!strcmp(st, "startup")) { snprintf(out, n, "Klipper is starting..."); return; }
    char msg[480];
    ks_klippy_message(msg, sizeof msg);
    snprintf(out, n, "%s", msg[0] ? msg : "Initializing printer...");
}
static void on_state(lv_event_t*) {
    if (!g_text) return;
    char b[500]; splash_text(b, sizeof b);
    lv_label_set_text(g_text, b);
}
static void on_del(lv_event_t*) { g_text = nullptr; }

static void on_restart(lv_event_t*) { ks_post("/printer/restart", "{}"); ks_toast("Klipper restart sent"); }
static void on_fw_restart(lv_event_t*) { ks_post("/printer/firmware_restart", "{}"); ks_toast("Firmware restart sent"); }
static void on_menu(lv_event_t*) { ks_menu_open_splash(); }
static void on_retry(lv_event_t*) { ks_retry(); ks_toast("Retrying..."); }

void ks_panel_splash(lv_obj_t* c) {
    lv_obj_t* info = lv_obj_create(c);
    lv_obj_set_size(info, lv_pct(100), 270);
    lv_obj_set_style_bg_opa(info, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(info, 0, 0);
    lv_obj_set_style_pad_all(info, 8, 0);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(info, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(info, LV_OBJ_FLAG_SCROLLABLE);
    ks_img(info, "klipper", 128, lv_color_white());
    g_text = ks_label(info, "", &ks_font_22);
    lv_label_set_long_mode(g_text, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_text, 480);
    lv_obj_set_style_pad_left(g_text, 16, 0);
    lv_obj_add_event_cb(info, on_state, LV_EVENT_MSG_RECEIVED, nullptr);
    lv_obj_add_event_cb(info, on_del, LV_EVENT_DELETE, nullptr);
    lv_msg_subscribe_obj(KS_MSG_STATE, info, nullptr);
    on_state(nullptr);

    lv_obj_t* row = lv_obj_create(c);                       // .actions: homogeneous button row
    lv_obj_set_size(row, lv_pct(100), 140);
    lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_gap(row, 4, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    struct B { const char* icon; const char* name; int color; lv_event_cb_t cb; };
    const B bs[] = {{"refresh", "Klipper Restart", 1, on_restart}, {"refresh", "Firmware Restart", 2, on_fw_restart},
                    {"settings", "Menu", 4, on_menu}, {"load", "Retry", 3, on_retry}};
    for (const B& b : bs) {
        lv_obj_t* btn = ks_button_sized(row, b.icon, 56, b.name, b.color, b.cb, nullptr);
        lv_obj_set_flex_grow(btn, 1);
        lv_obj_set_height(btn, lv_pct(100));
    }
}

// panels/settings.py (menu "KlipperScreen"): list of options, one frame-item row each.
// Rows so far: Confirm Emergency Stop, 24 Hour Time (official options), Brightness, Refresh rate (Panda Touch only).
#include <Arduino.h>
#include "ks_widgets.h"
#include "ks_cfg.h"
#include "ks_panels.h"
#include "../conf/global_config.h"

uint32_t pt_pclk_khz();                 // ESP32-PANDATOUCH.cpp
void pt_pclk_set_khz(uint32_t khz);
void screen_setBrightness(unsigned char b);

static lv_obj_t* switch_row(lv_obj_t* list, const char* name, const char* key, bool def) {
    lv_obj_t* r = ks_row(list, name);
    ks_switch(ks_row_right(r, 120), ks_cfg_bool(key, def), [](lv_event_t* e) {
        lv_obj_t* s = lv_event_get_target(e);
        ks_cfg_set_bool((const char*)lv_event_get_user_data(e), lv_obj_has_state(s, LV_STATE_CHECKED));
    }, (void*)key);
    return r;
}

// ---- brightness (20-255, stored in the CYD config, applied at once) ---------------------------------------------------------
static void brightness_row(lv_obj_t* list) {
    lv_obj_t* r = ks_row(list, "Brightness");
    lv_obj_t* box = ks_row_right(r, 320);
    ks_slider(box, 20, 255, global_config.brightness < 20 ? 255 : global_config.brightness, 300, [](lv_event_t* e) {
        int v = lv_slider_get_value(lv_event_get_target(e));
        global_config.brightness = v;
        write_global_config();
        screen_setBrightness(v);
    }, nullptr);
}

// ---- refresh rate (pixel clock) -----------------------------------------------------------------------------------------------
// Total frame = (800+4+4+8) x (480+4+12+16) pixel clocks; the user trims the clock until the picture holds still.
static lv_obj_t* g_clk_lbl;
static lv_timer_t* g_apply;
static uint32_t g_khz;

static void clk_text() {
    char b[48];
    snprintf(b, sizeof b, "%.1f MHz  (%.0f Hz)", g_khz / 1000.0, g_khz * 1000.0 / (816.0 * 512.0));
    lv_label_set_text(g_clk_lbl, b);
}
static void apply_cb(lv_timer_t* t) {
    lv_timer_del(t);
    g_apply = nullptr;
    ks_toast("Applying new refresh rate...");
    lv_refr_now(nullptr);
    delay(400);
    ESP.restart();
}
static void clk_step(lv_event_t* e) {
    int step = (int)(intptr_t)lv_event_get_user_data(e);
    int v = (int)g_khz + step;
    if (v < 12000) v = 12000;     // panel datasheet: DCLK 23 / 25 / 27 MHz (min / typ / max); below 23 is out of spec but BTT ships 14.8
    if (v > 27000) v = 27000;
    g_khz = (uint32_t)v;
    pt_pclk_set_khz(g_khz);
    clk_text();
    // restart once the user stops tapping, so several taps cost one reboot
    if (g_apply) lv_timer_reset(g_apply); else g_apply = lv_timer_create(apply_cb, 1800, nullptr);
}

static void refresh_row(lv_obj_t* list) {
    g_khz = pt_pclk_khz();
    lv_obj_t* r = ks_row(list, "Refresh rate", "Pixel clock. 14.5 MHz is clean on this panel. Touch the screen while it boots to reset.");
    lv_obj_t* box = ks_row_right(r, 290);
    lv_obj_t* minus = ks_button(box, "decrease", nullptr, 1, clk_step, (void*)(intptr_t)-500);
    lv_obj_set_size(minus, 72, 64);
    g_clk_lbl = ks_label(box, "", &ks_font_22);
    lv_obj_set_width(g_clk_lbl, 130);
    lv_obj_set_style_text_align(g_clk_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t* plus = ks_button(box, "increase", nullptr, 3, clk_step, (void*)(intptr_t)500);
    lv_obj_set_size(plus, 72, 64);
    lv_obj_add_event_cb(box, [](lv_event_t*) { g_clk_lbl = nullptr; }, LV_EVENT_DELETE, nullptr);
    clk_text();
}

void ks_panel_settings(lv_obj_t* c) {
    lv_obj_t* l = ks_list(c);
    switch_row(l, "Confirm Emergency Stop", "confirm_estop", false);
    switch_row(l, "24 Hour Time", "24htime", true);
    brightness_row(l);
    refresh_row(l);
}

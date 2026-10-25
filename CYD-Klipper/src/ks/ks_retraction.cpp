// panels/retraction.py: sliders for the firmware_retraction options (value read from the status, defaults/limits from the
// configfile) with a reset button each; releasing a slider sends SET_RETRACTION. GET_RETRACTION is sent on open.
#include <Arduino.h>
#include "ks_widgets.h"
#include "ks_mr.h"
#include "ks_panels.h"

namespace {
struct Opt {
    const char *name, *units, *key;
    int scale;                // slider units per real unit (100 -> 2 digits)
    float def, maxv, lo;      // config value (reset target), slider max, slider min
    lv_obj_t *slider, *val;
};
struct Ctx { Opt o[4]; lv_obj_t* note = nullptr; };
Ctx* g = nullptr;

void show(Opt& o, int v) {
    char b[16];
    if (o.scale == 1) snprintf(b, sizeof b, "%d", v); else snprintf(b, sizeof b, "%.2f", v / (float)o.scale);
    lv_label_set_text(o.val, b);
}
void send(Opt& o, int v) {
    char key[32]; strlcpy(key, o.key, sizeof key);
    for (char* p = key; *p; p++) *p = toupper(*p);
    if (o.scale == 1) ks_gcode("SET_RETRACTION %s=%d", key, v);
    else ks_gcode("SET_RETRACTION %s=%.2f", key, v / (float)o.scale);
}
void on_release(lv_event_t* e) { Opt& o = *(Opt*)lv_event_get_user_data(e); send(o, lv_slider_get_value(o.slider)); }
void on_drag(lv_event_t* e) { Opt& o = *(Opt*)lv_event_get_user_data(e); show(o, lv_slider_get_value(o.slider)); }

void set_value(Opt& o, float real) {                             // "infinite scale": range follows the value
    int v = (int)lroundf(real * o.scale);
    int upper = real > o.maxv * .75f ? (int)(real * 1.5f * o.scale) : (int)(o.maxv * o.scale);
    lv_slider_set_range(o.slider, (int)(o.lo * o.scale), upper);
    lv_slider_set_value(o.slider, v, LV_ANIM_OFF);
    show(o, v);
}
void on_reset(lv_event_t* e) {
    Opt& o = g->o[(intptr_t)lv_event_get_user_data(e)];
    set_value(o, o.def);
    send(o, (int)lroundf(o.def * o.scale));
}
void on_state(lv_event_t*) {
    if (!g || !ks_has("firmware_retraction")) return;
    for (Opt& o : g->o) {
        if (lv_obj_has_state(o.slider, LV_STATE_PRESSED)) continue;
        set_value(o, ks_f("firmware_retraction", o.key, o.def));
    }
}
void got_config(bool ok, JsonDocument& doc, void*) {
    if (!ok || !g) return;
    JsonObjectConst c = doc["result"]["status"]["configfile"]["settings"]["firmware_retraction"];
    if (c.isNull()) return;
    for (Opt& o : g->o) {
        if (c[o.key].is<float>()) o.def = c[o.key].as<float>();
        bool spd = o.scale == 1;
        o.maxv = o.def * 1.2f >= (spd ? 100 : 6) ? o.def * 1.2f : (spd ? 100 : 6);   // maxlength / maxspeed of the Python
        if (!lv_obj_has_state(o.slider, LV_STATE_PRESSED)) set_value(o, ks_f("firmware_retraction", o.key, o.def));
    }
}
}  // namespace

void ks_panel_retraction(lv_obj_t* c) {
    g = new Ctx{{
        {"Retraction Length", "mm", "retract_length", 100, 0, 6, 0},
        {"Retraction Speed", "mm/s", "retract_speed", 1, 20, 100, 1},
        {"Unretract Extra Length", "mm", "unretract_extra_length", 100, 0, 6, 0},
        {"Unretract Speed", "mm/s", "unretract_speed", 1, 10, 100, 1}}};
    lv_obj_t* list = ks_list(c);
    lv_obj_add_event_cb(list, [](lv_event_t*) { delete g; g = nullptr; }, LV_EVENT_DELETE, nullptr);
    if (!ks_has("firmware_retraction")) {
        g->note = ks_label(list, "[firmware_retraction] is not configured on this printer.", &ks_font_22);
        lv_obj_set_style_text_color(g->note, KS_WARNING, 0);
        lv_label_set_long_mode(g->note, LV_LABEL_LONG_WRAP);
        lv_obj_set_width(g->note, lv_pct(100));
    }
    for (int i = 0; i < 4; i++) {
        Opt& o = g->o[i];
        lv_obj_t* blk = ks_card(list, lv_pct(100), 112);
        lv_obj_set_flex_flow(blk, LV_FLEX_FLOW_COLUMN);
        char t[64]; snprintf(t, sizeof t, "%s (%s)", o.name, o.units);
        lv_obj_t* l = ks_label(blk, t, &ks_font_28);
        lv_obj_set_height(l, 40);
        lv_obj_t* row = ks_card(blk, lv_pct(100), 64);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 8, 0);
        o.slider = ks_slider(row, (int)(o.lo * o.scale), (int)(o.maxv * o.scale), 0, 450, on_release, &o);
        lv_obj_add_event_cb(o.slider, on_drag, LV_EVENT_VALUE_CHANGED, &o);
        o.val = ks_label(row, "", &ks_font_22);
        lv_obj_set_width(o.val, 80);
        lv_obj_set_style_text_align(o.val, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_t* rs = ks_button_sized(row, "refresh", 40, nullptr, 1, on_reset, (void*)(intptr_t)i);
        lv_obj_set_size(rs, 72, 60);
        set_value(o, o.def);
    }
    ks_on_state(list, on_state);
    on_state(nullptr);
    ks_get_async("/printer/objects/query?configfile=settings", got_config, nullptr);
    if (ks_has("firmware_retraction")) ks_gcode("GET_RETRACTION");
}

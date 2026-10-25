// panels/limits.py: sliders for Max Acceleration, Max Acceleration to Deceleration (old Klipper) or Minimum Cruise Ratio (new
// Klipper, what the SV08 reports), Max Velocity, Square Corner Velocity, each with a reset-to-config button; SET_VELOCITY_LIMIT.
// Addition vs the Python: Pressure Advance (SET_PRESSURE_ADVANCE) when an extruder exists.
#include <Arduino.h>
#include "ks_widgets.h"
#include "ks_mr.h"
#include "ks_panels.h"

namespace {
struct Opt {
    const char *name, *units, *obj, *field, *cmd;   // cmd: format taking the value
    int scale;                                      // slider units per real unit
    float cfg, lo, hardmax;                         // config value (slider max, reset target), slider minimum, absolute cap (0 = none)
    lv_obj_t *slider, *val;
};
struct Ctx { Opt o[5]; int n = 0; };
Ctx* g = nullptr;

void show(Opt& o, int v) {
    char b[16];
    if (o.scale == 1) snprintf(b, sizeof b, "%d", v); else snprintf(b, sizeof b, o.scale == 100 ? "%.2f" : "%.3f", v / (float)o.scale);
    lv_label_set_text(o.val, b);
}
void send(Opt& o, int v) {
    char val[16], cmd[96];
    if (o.scale == 1) snprintf(val, sizeof val, "%d", v); else snprintf(val, sizeof val, o.scale == 100 ? "%.2f" : "%.3f", v / (float)o.scale);
    snprintf(cmd, sizeof cmd, o.cmd, val);
    ks_gcode("%s", cmd);
}
void set_value(Opt& o, float real) {                  // range top = 1.5 x config, or 1.5 x value when above the config
    float top = real > o.cfg ? real * 1.5f : o.cfg * 1.5f;
    if (o.hardmax > 0 && top > o.hardmax) top = o.hardmax;
    lv_slider_set_range(o.slider, (int)(o.lo * o.scale), (int)lroundf(top * o.scale));
    int v = (int)lroundf(real * o.scale);
    lv_slider_set_value(o.slider, v, LV_ANIM_OFF);
    show(o, v);
    lv_obj_set_style_bg_color(o.slider, real > o.cfg ? KS_WARNING : KS_BLUE, LV_PART_INDICATOR);   // .option_slider_max
}
void on_release(lv_event_t* e) { Opt& o = *(Opt*)lv_event_get_user_data(e); send(o, lv_slider_get_value(o.slider)); }
void on_drag(lv_event_t* e) { Opt& o = *(Opt*)lv_event_get_user_data(e); show(o, lv_slider_get_value(o.slider)); }
void on_reset(lv_event_t* e) {
    Opt& o = g->o[(intptr_t)lv_event_get_user_data(e)];
    set_value(o, o.cfg);
    send(o, (int)lroundf(o.cfg * o.scale));
}
void on_state(lv_event_t*) {
    if (!g) return;
    for (int i = 0; i < g->n; i++) {
        Opt& o = g->o[i];
        if (lv_obj_has_state(o.slider, LV_STATE_PRESSED)) continue;
        float v = ks_f(o.obj, o.field, NAN);
        if (!isnan(v)) set_value(o, v);
    }
}
void got_config(bool ok, JsonDocument& doc, void*) {
    if (!ok || !g) return;
    JsonObjectConst st = doc["result"]["status"]["configfile"]["settings"];
    for (int i = 0; i < g->n; i++) {
        Opt& o = g->o[i];
        JsonVariantConst v = st[!strcmp(o.obj, "toolhead") ? "printer" : o.obj][o.field];
        if (v.is<float>()) o.cfg = v.as<float>();
        else if (!strcmp(o.field, "max_accel_to_decel")) o.cfg = st["printer"]["max_accel"].as<float>() / 2;   // Klipper default
        if (!lv_obj_has_state(o.slider, LV_STATE_PRESSED)) { float c = ks_f(o.obj, o.field, o.cfg); set_value(o, c); }
    }
}
}  // namespace

void ks_panel_limits(lv_obj_t* c) {
    g = new Ctx();
    auto add = [](const char* name, const char* units, const char* obj, const char* field, const char* cmd, int scale, float lo, float hardmax) {
        float cur = ks_f(obj, field, NAN);
        if (isnan(cur)) return;                                           // option not reported by this Klipper
        g->o[g->n++] = Opt{name, units, obj, field, cmd, scale, cur, lo, hardmax, nullptr, nullptr};
    };
    add("Max Acceleration", "mm/s\xC2\xB2", "toolhead", "max_accel", "SET_VELOCITY_LIMIT ACCEL=%s", 1, 1, 0);
    if (!isnan(ks_f("toolhead", "max_accel_to_decel", NAN)))
        add("Max Acceleration to Deceleration", "mm/s\xC2\xB2", "toolhead", "max_accel_to_decel", "SET_VELOCITY_LIMIT ACCEL_TO_DECEL=%s", 1, 1, 0);
    else
        add("Minimum Cruise Ratio", "", "toolhead", "minimum_cruise_ratio", "SET_VELOCITY_LIMIT MINIMUM_CRUISE_RATIO=%s", 100, 0, 1);
    add("Max Velocity", "mm/s", "toolhead", "max_velocity", "SET_VELOCITY_LIMIT VELOCITY=%s", 1, 1, 0);
    add("Square Corner Velocity", "mm/s", "toolhead", "square_corner_velocity", "SET_VELOCITY_LIMIT SQUARE_CORNER_VELOCITY=%s", 1, 1, 0);
    add("Pressure Advance", "s", "extruder", "pressure_advance", "SET_PRESSURE_ADVANCE ADVANCE=%s", 1000, 0, 0);

    lv_obj_t* list = ks_list(c);
    lv_obj_add_event_cb(list, [](lv_event_t*) { delete g; g = nullptr; }, LV_EVENT_DELETE, nullptr);
    for (int i = 0; i < g->n; i++) {
        Opt& o = g->o[i];
        lv_obj_t* blk = ks_card(list, lv_pct(100), 112);
        lv_obj_set_flex_flow(blk, LV_FLEX_FLOW_COLUMN);
        char t[64];
        if (*o.units) snprintf(t, sizeof t, "%s (%s)", o.name, o.units); else strlcpy(t, o.name, sizeof t);
        lv_obj_t* l = ks_label(blk, t, &ks_font_28);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_set_size(l, lv_pct(100), 40);
        lv_obj_t* row = ks_card(blk, lv_pct(100), 64);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 8, 0);
        o.slider = ks_slider(row, 0, 100, 0, 450, on_release, &o);
        lv_obj_add_event_cb(o.slider, on_drag, LV_EVENT_VALUE_CHANGED, &o);
        o.val = ks_label(row, "", &ks_font_22);
        lv_obj_set_width(o.val, 80);
        lv_obj_set_style_text_align(o.val, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_t* rs = ks_button_sized(row, "refresh", 40, nullptr, 1, on_reset, (void*)(intptr_t)i);
        lv_obj_set_size(rs, 72, 60);
        set_value(o, o.cfg);
    }
    ks_on_state(list, on_state);
    ks_get_async("/printer/objects/query?configfile=settings", got_config, nullptr);
}

// panels/fan.py: one block per fan, name on top. fan / fan_generic: [stop] [slider 0-100] [Max]; heater_fan / controller_fan:
// read-only bar. "fan" -> M106 S<0-255>, others -> SET_FAN_SPEED FAN=<name> SPEED=<0-1>. Names starting with "_" are hidden.
#include <Arduino.h>
#include <algorithm>
#include "ks_widgets.h"
#include "ks_mr.h"
#include "ks_panels.h"

namespace {
struct Fan { char obj[40]; bool changeable; lv_obj_t *slider, *bar, *val; };
struct Ctx { Fan f[10]; int n = 0; };
Ctx* g = nullptr;

void show(Fan& f, int pct) {
    char b[8]; snprintf(b, sizeof b, "%d%%", pct);
    lv_label_set_text(f.val, b);
}
void send(Fan& f, int pct) {
    if (!strcmp(f.obj, "fan")) ks_gcode("M106 S%.0f", pct * 2.55f);
    else ks_gcode("SET_FAN_SPEED FAN=%s SPEED=%.2f", strchr(f.obj, ' ') + 1, pct / 100.0f);
}
void on_release(lv_event_t* e) {
    Fan& f = *(Fan*)lv_event_get_user_data(e);
    send(f, lv_slider_get_value(f.slider));
}
void on_set(lv_event_t* e) {                                   // stop (0) / Max (100)
    intptr_t v = (intptr_t)lv_event_get_user_data(e);
    Fan& f = g->f[v >> 8];
    int pct = v & 0xFF;
    lv_slider_set_value(f.slider, pct, LV_ANIM_OFF);
    show(f, pct);
    send(f, pct);
}
void on_drag(lv_event_t* e) {
    Fan& f = *(Fan*)lv_event_get_user_data(e);
    show(f, lv_slider_get_value(f.slider));
}
void on_state(lv_event_t*) {
    if (!g) return;
    for (int i = 0; i < g->n; i++) {
        Fan& f = g->f[i];
        int pct = (int)lroundf(ks_f(f.obj, "speed") * 100);
        if (f.changeable) {
            if (lv_obj_has_state(f.slider, LV_STATE_PRESSED)) continue;       // do not fight the finger
            lv_slider_set_value(f.slider, pct, LV_ANIM_OFF);
        } else lv_bar_set_value(f.bar, pct, LV_ANIM_OFF);
        show(f, pct);
    }
}
}  // namespace

void ks_panel_fan(lv_obj_t* c) {
    g = new Ctx();
    lv_obj_t* list = ks_list(c);
    lv_obj_add_event_cb(list, [](lv_event_t*) { delete g; g = nullptr; }, LV_EVENT_DELETE, nullptr);

    char (*nm)[40] = (char(*)[40])malloc(24 * 40);
    if (ks_has("fan") && g->n < 10) { strcpy(g->f[g->n].obj, "fan"); g->f[g->n++].changeable = true; }
    if (nm) {
        static const char* const pre[] = {"controller_fan ", "fan_generic ", "heater_fan "};
        char all[3 * 8][40]; int na = 0;
        for (auto p : pre) {
            int n = ks_objects(p, nm, 8);
            for (int i = 0; i < n && na < 24; i++) if (nm[i][strlen(p)] != '_') strlcpy(all[na++], nm[i], 40);
        }
        qsort(all, na, 40, [](const void* a, const void* b) { return strcmp(strchr((const char*)a, ' ') + 1, strchr((const char*)b, ' ') + 1); });
        for (int i = 0; i < na && g->n < 10; i++) {
            strlcpy(g->f[g->n].obj, all[i], 40);
            g->f[g->n++].changeable = !strncmp(all[i], "fan_generic ", 12);
        }
        free(nm);
    }

    for (int i = 0; i < g->n; i++) {
        Fan& f = g->f[i];
        lv_obj_t* blk = ks_card(list, lv_pct(100), 112);
        lv_obj_set_flex_flow(blk, LV_FLEX_FLOW_COLUMN);
        const char* nm_ = strcmp(f.obj, "fan") ? strchr(f.obj, ' ') + 1 : "Part Fan";
        lv_obj_t* l = ks_label(blk, nm_, &ks_font_28);
        lv_obj_set_height(l, 40);
        lv_obj_t* row = ks_card(blk, lv_pct(100), 64);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 8, 0);
        if (f.changeable) {
            lv_obj_t* stop = ks_button_sized(row, "cancel", 40, nullptr, 1, on_set, (void*)(intptr_t)((i << 8) | 0));
            lv_obj_set_size(stop, 72, 60);
            f.slider = ks_slider(row, 0, 100, 0, 380, on_release, &f);
            lv_obj_add_event_cb(f.slider, on_drag, LV_EVENT_VALUE_CHANGED, &f);
            lv_obj_t* mx = ks_button_sized(row, "fan-on", 40, "Max", 2, on_set, (void*)(intptr_t)((i << 8) | 100));
            lv_obj_set_size(mx, 100, 60);
        } else {
            f.bar = lv_bar_create(row);
            lv_obj_set_size(f.bar, 480, 30);
            lv_bar_set_range(f.bar, 0, 100);
            lv_obj_set_style_bg_color(f.bar, KS_ACTIVE, LV_PART_MAIN);
            lv_obj_set_style_bg_color(f.bar, KS_BLUE, LV_PART_INDICATOR);
        }
        f.val = ks_label(row, "", &ks_font_22);
        lv_obj_set_width(f.val, 70);
        lv_obj_set_style_text_align(f.val, LV_TEXT_ALIGN_RIGHT, 0);
    }
    ks_on_state(list, on_state);
    on_state(nullptr);
}

// panels/extrude.py: extruder selector (T<n> buttons when several), Temperature / Spoolman shortcuts, Extrude / Load / Unload /
// Retract, distance + speed toggles, filament sensor switches. LOAD_FILAMENT / UNLOAD_FILAMENT are macros, SPEED is mm/s * 60.
// Additions vs the Python: "Extruder too cold" toast for plain extrude/retract (Klipper would reject it with an error anyway).
#include <Arduino.h>
#include <strings.h>
#include "ks_widgets.h"
#include "ks_mr.h"
#include "ks_panels.h"

namespace {
const char* const DISTANCES[] = {"5", "10", "15", "25"};
const char* const SPEEDS[] = {"1", "2", "5", "25"};      // Sovol's extrude.py defaults (not 10)

struct Sensor { char obj[48], name[40]; lv_obj_t *box, *sw; };
struct Ctx {
    int dist = 1, speed = 1;
    bool has_load = false, has_unload = false;
    int next = 0;
    char ext[6][40];
    int n_ext = 0;
    lv_obj_t *ext_btn[6], *ext_lbl[6], *xbox, *action[4], *spool = nullptr;
    Sensor fs[4];
    int n_fs = 0;
};
Ctx* g = nullptr;

int tool_no(const char* name) { return name[8] ? atoi(name + 8) : 0; }

bool can_use() {                                          // Python: printer.state in ("ready", "paused")
    char st[16] = "";
    ks_str("print_stats", "state", st, sizeof st);
    return !strcmp(ks_klippy_state(), "ready") && strcmp(st, "printing") != 0;
}

void do_extrude(lv_event_t* e) {
    int dir = (int)(intptr_t)lv_event_get_user_data(e);
    if (!ks_b("extruder", "can_extrude", true)) { ks_toast("Extruder too cold"); return; }
    ks_gcode("M83");
    ks_gcode("G1 E%c%s F%d", dir > 0 ? '+' : '-', DISTANCES[g->dist], atoi(SPEEDS[g->speed]) * 60);
}
void do_load(lv_event_t* e) {
    bool load = (intptr_t)lv_event_get_user_data(e) > 0;
    if (!(load ? g->has_load : g->has_unload)) { ks_toast(load ? "Macro LOAD_FILAMENT not found" : "Macro UNLOAD_FILAMENT not found"); return; }
    ks_gcode("%s SPEED=%d", load ? "LOAD_FILAMENT" : "UNLOAD_FILAMENT", atoi(SPEEDS[g->speed]) * 60);
}
void do_tool(lv_event_t* e) { ks_gcode("T%d", tool_no(g->ext[(intptr_t)lv_event_get_user_data(e)])); }
void do_temp(lv_event_t*) { ks_open("Temperature", ks_panel_temperature); }
void do_spool(lv_event_t*) { ks_open("Spoolman", ks_panel_spoolman); }

void on_state(lv_event_t*) {
    if (!g) return;
    char cur[40] = "extruder";
    ks_str("toolhead", "extruder", cur, sizeof cur);
    for (int i = 0; i < g->n_ext; i++) {
        float t = ks_f(g->ext[i], "temperature"), tg = ks_f(g->ext[i], "target");
        char b[40];
        if (tg > 0) snprintf(b, sizeof b, "%d/%d", (int)t, (int)tg); else snprintf(b, sizeof b, "%d\xC2\xB0", (int)t);
        if (g->n_ext > 1) { char c[48]; snprintf(c, sizeof c, "T%d\n%s", tool_no(g->ext[i]), b); lv_label_set_text(g->ext_lbl[i], c); }
        else lv_label_set_text(g->ext_lbl[i], b);
        lv_obj_set_style_bg_color(g->ext_btn[i], !strcmp(cur, g->ext[i]) ? KS_ACTIVE : KS_BG, 0);
    }
    bool en = can_use();
    for (int i = 0; i < 4; i++) {
        if (en) lv_obj_clear_state(g->action[i], LV_STATE_DISABLED); else lv_obj_add_state(g->action[i], LV_STATE_DISABLED);
    }
    for (int i = 0; i < g->n_fs; i++) {
        Sensor& s = g->fs[i];
        bool on = ks_b(s.obj, "enabled", false);
        if (on != lv_obj_has_state(s.sw, LV_STATE_CHECKED)) { if (on) lv_obj_add_state(s.sw, LV_STATE_CHECKED); else lv_obj_clear_state(s.sw, LV_STATE_CHECKED); }
        lv_color_t c = !on ? KS_BG : ks_b(s.obj, "filament_detected", false) ? KS_ECHO : KS_ERROR;   // detected / empty
        lv_obj_set_style_bg_color(s.box, c, 0);
    }
}

void spool_cb(bool ok, JsonDocument& doc, void*) {                            // KS shows the button when printer.spoolman
    if (!ok || !g || g->spool || !doc["result"]["spoolman_connected"].as<bool>()) return;
    g->spool = ks_button_sized(g->xbox, "spoolman", 40, "Spoolman", 3, do_spool, nullptr);
    lv_obj_set_flex_grow(g->spool, 1); lv_obj_set_height(g->spool, lv_pct(100));
}

lv_obj_t* titled(lv_obj_t* parent, const char* title, int w) {
    lv_obj_t* box = ks_card(parent, w, lv_pct(100));
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_t* l = ks_label(box, title, &ks_font_22);
    lv_obj_set_width(l, lv_pct(100));
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_height(l, 30);
    return box;
}
}  // namespace

void ks_panel_extrude(lv_obj_t* c) {
    g = new Ctx();
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(c, 2, 0);
    lv_obj_set_style_pad_row(c, 4, 0);
    lv_obj_add_event_cb(c, [](lv_event_t*) { delete g; g = nullptr; }, LV_EVENT_DELETE, nullptr);

    char (*names)[40] = (char(*)[40])malloc(128 * 40);
    if (names) {
        int n = ks_macros(names, 128);
        for (int i = 0; i < n; i++) {
            if (strcasestr(names[i], "LOAD_FILAMENT")) g->has_load = true;       // same substring test as the Python
            if (strcasestr(names[i], "UNLOAD_FILAMENT")) g->has_unload = true;
        }
        int ne = ks_objects("extruder", names, 128);
        for (int i = 0; i < ne && g->n_ext < 6; i++) {
            const char* s = names[i] + 8; bool ok = true;
            for (; *s; s++) if (*s < '0' || *s > '9') ok = false;
            if (ok) strlcpy(g->ext[g->n_ext++], names[i], 40);
        }
        int nf = ks_objects("filament_switch_sensor ", names, 8), nm = ks_objects("filament_motion_sensor ", names + nf, 8);
        for (int i = 0; i < nf + nm && g->n_fs < 4; i++) {
            Sensor& s = g->fs[g->n_fs++];
            strlcpy(s.obj, names[i], sizeof s.obj);
            strlcpy(s.name, strchr(names[i], ' ') + 1, sizeof s.name);
        }
        free(names);
    }
    for (int i = 0; i < g->n_ext - 1; i++)                   // sort by tool number (bubble, n <= 6)
        for (int j = 0; j < g->n_ext - 1 - i; j++)
            if (tool_no(g->ext[j]) > tool_no(g->ext[j + 1])) { char t[40]; strcpy(t, g->ext[j]); strcpy(g->ext[j], g->ext[j + 1]); strcpy(g->ext[j + 1], t); }

    g->xbox = ks_card(c, lv_pct(100), 92);                     // extruder(s) + Temperature (+ Spoolman)
    lv_obj_set_flex_flow(g->xbox, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(g->xbox, 2, 0);
    for (int i = 0; i < g->n_ext; i++) {
        char icon[16]; snprintf(icon, sizeof icon, "extruder-%d", tool_no(g->ext[i]));
        lv_obj_t* b = ks_button_sized(g->xbox, g->n_ext == 1 ? "extruder" : icon, 40, nullptr, 0,
                                      g->n_ext > 1 ? do_tool : nullptr, (void*)(intptr_t)i);
        g->ext_lbl[i] = ks_label(b, "", &ks_font_22);
        lv_obj_set_style_text_align(g->ext_lbl[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_flex_grow(b, 1); lv_obj_set_height(b, lv_pct(100));
        g->ext_btn[i] = b;
    }
    lv_obj_t* tb = ks_button_sized(g->xbox, "heat-up", 40, "Temperature", 4, do_temp, nullptr);
    lv_obj_set_flex_grow(tb, 1); lv_obj_set_height(tb, lv_pct(100));

    lv_obj_t* act = ks_card(c, lv_pct(100), 120);              // Extrude | Load | Unload | Retract
    lv_obj_set_flex_flow(act, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(act, 2, 0);
    struct { const char* icon; const char* label; int color; lv_event_cb_t cb; int dir; } a[4] = {
        {"extrude", "Extrude", 4, do_extrude, 1}, {"arrow-down", "Load", 3, do_load, 1},
        {"arrow-up", "Unload", 2, do_load, -1}, {"retract", "Retract", 1, do_extrude, -1}};
    for (int i = 0; i < 4; i++) {
        g->action[i] = ks_button_sized(act, a[i].icon, 56, a[i].label, a[i].color, a[i].cb, (void*)(intptr_t)a[i].dir);
        lv_obj_set_flex_grow(g->action[i], 1); lv_obj_set_height(g->action[i], lv_pct(100));
    }

    lv_obj_t* ds = ks_card(c, lv_pct(100), 96);
    lv_obj_set_flex_flow(ds, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(ds, 4, 0);
    lv_obj_t* db = titled(ds, "Distance (mm)", 354);
    ks_toggles(db, DISTANCES, 4, g->dist, [](int i, void*) { g->dist = i; }, nullptr);
    lv_obj_t* sb = titled(ds, "Speed (mm/s)", 354);
    ks_toggles(sb, SPEEDS, 4, g->speed, [](int i, void*) { g->speed = i; }, nullptr);

    if (g->n_fs) {                                              // .filament_sensor boxes with a switch each
        lv_obj_t* row = ks_card(c, lv_pct(100), 64);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(row, 4, 0);
        for (int i = 0; i < g->n_fs; i++) {
            Sensor& s = g->fs[i];
            s.box = ks_card(row, 170, 60);
            lv_obj_set_style_bg_opa(s.box, LV_OPA_COVER, 0);
            lv_obj_set_flex_flow(s.box, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(s.box, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_all(s.box, 6, 0);
            lv_obj_t* l = ks_label(s.box, s.name, &ks_font_18);
            lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
            lv_obj_set_width(l, 60);
            s.sw = ks_switch(s.box, false, [](lv_event_t* e) {
                Sensor* s = (Sensor*)lv_event_get_user_data(e);
                ks_gcode("SET_FILAMENT_SENSOR SENSOR=%s ENABLE=%d", s->name, lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED) ? 1 : 0);
            }, &s);
            lv_obj_set_size(s.sw, 80, 44);
        }
    }
    ks_on_state(c, on_state);
    on_state(nullptr);
    ks_get_async("/server/spoolman/status", spool_cb, nullptr);
}

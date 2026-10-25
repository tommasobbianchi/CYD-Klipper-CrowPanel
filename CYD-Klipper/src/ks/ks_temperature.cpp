// panels/temperature.py: device list (current/target, tap = select, long press = show/hide in graph), temperature graph
// (last 3 min sampled once per status update, z-bolt graph colours), Cooldown, preheat grid (defaults.conf [preheat *]) or
// +/- target adjust with delta toggles ("fine-tune" button switches between the two). Not ported: on-screen numpad / PID calibrate.
#include <Arduino.h>
#include <algorithm>
#include <ctype.h>
#include <strings.h>
#include "ks_widgets.h"
#include "ks_mr.h"
#include "ks_cfg.h"
#include "ks_panels.h"

namespace {
enum Kind { EXT, BED, GENERIC, TFAN, SENSOR };
struct Dev {
    char name[40];
    uint8_t kind;
    bool target, sel, vis;
    int max_temp;
    lv_color_t col;
    lv_obj_t *row, *nm, *tv;
    lv_chart_series_t* ser;
};
constexpr int MAXDEV = 12, POINTS = 180;
struct Ctx {
    Dev d[MAXDEV];
    int n = 0;
    bool preheat = false;
    int delta = 2;                       // tempdeltas[-2] = 10
    lv_obj_t *root, *right, *chart, *ymax;
    int ytop = 100;
};
Ctx* g = nullptr;

const char* const DELTAS[] = {"1", "5", "10", "25"};
struct Preset { const char* name; int bed, ext; };
const Preset PRESETS[] = {{"PLA", 40, 195}, {"ABS", 90, 220}, {"PETG", 80, 240}, {"FLEX", 0, 210}};   // defaults.conf

const char* const EXT_COL[] = {"ED6500", "ED8500", "EDA500", "EDC500", "EDF500"};
const char* const FAN_COL[] = {"A7E100", "7ECE21", "AAA920", "AC7140"};
const char* const SEN_COL[] = {"B10080", "7800B0", "AC4958", "B1A080", "B1C080"};
lv_color_t hex(const char* s) { return lv_color_hex(strtol(s, nullptr, 16)); }

void pretty(const char* full, char* out, size_t n) {          // ScreenPanel.prettify: "my_sensor" -> "My Sensor"
    const char* s = strchr(full, ' ');
    s = s ? s + 1 : full;
    bool up = true; size_t i = 0;
    for (; *s && i + 1 < n; s++, i++) {
        out[i] = *s == '_' ? ' ' : (up ? toupper(*s) : *s);
        up = out[i] == ' ';
    }
    out[i] = 0;
}
int tool_no(const char* name) { return name[8] ? atoi(name + 8) : 0; }

bool printing() {
    char st[16] = "";
    ks_str("print_stats", "state", st, sizeof st);
    return !strcmp(st, "printing") || !strcmp(st, "paused");
}

void set_target(const Dev& d, int t) {
    const char* sp = strchr(d.name, ' ');
    switch (d.kind) {
        case EXT: ks_gcode("M104 T%d S%d", tool_no(d.name), t); break;
        case BED: ks_gcode("M140 S%d", t); break;
        case GENERIC: ks_gcode("SET_HEATER_TEMPERATURE heater=\"%s\" target=%d", sp + 1, t); break;
        case TFAN: ks_gcode("SET_TEMPERATURE_FAN_TARGET temperature_fan=\"%s\" target=%d", sp + 1, t); break;
    }
}
bool clamp_max(const Dev& d, int& t) {
    if (t > d.max_temp) { char b[48]; snprintf(b, sizeof b, "Can't set above the maximum: %d", d.max_temp); ks_toast(b); t = d.max_temp; return false; }
    return true;
}
bool any_selected() {
    for (int i = 0; i < g->n; i++) if (g->d[i].sel) return true;
    ks_toast("Nothing selected");
    return false;
}

// ---- device list ---------------------------------------------------------------------------------------------------------
void paint_row(Dev& d) {
    lv_obj_set_style_bg_color(d.row, d.sel ? KS_ACTIVE : KS_BG, 0);
    lv_obj_set_style_text_color(d.nm, d.vis ? d.col : lv_color_white(), 0);
}
void apply_vis() {
    int mask = 0;
    for (int i = 0; i < g->n; i++) {
        Dev& d = g->d[i];
        if (d.ser) lv_chart_hide_series(g->chart, d.ser, !d.vis);
        paint_row(d);
        if (!d.vis) mask |= 1 << i;
    }
    ks_cfg_set_int("graph_hide", mask);
}
void row_click(lv_event_t* e) {
    if (!g) return;
    Dev& d = g->d[(intptr_t)lv_event_get_user_data(e)];
    if (d.target) { d.sel = !d.sel; paint_row(d); }
    else { d.vis = !d.vis; apply_vis(); }
}
void row_long(lv_event_t* e) {
    if (!g) return;
    Dev& d = g->d[(intptr_t)lv_event_get_user_data(e)];
    d.vis = !d.vis;
    apply_vis();
}

void collect() {
    char (*nm)[40] = (char(*)[40])malloc(24 * 40);
    if (!nm) return;
    int ne = ks_objects("extruder", nm, 24), k = 0;
    char ext[8][40];
    for (int i = 0; i < ne && k < 8; i++) {
        const char* s = nm[i] + 8; bool ok = true;
        for (; *s; s++) if (!isdigit(*s)) ok = false;       // skips extruder_stepper
        if (ok) strlcpy(ext[k++], nm[i], 40);
    }
    qsort(ext, k, 40, [](const void* a, const void* b) { return tool_no((const char*)a) - tool_no((const char*)b); });
    int ei = 0, bi = 0, fi = 0, si = 0;
    auto add = [&](const char* name, Kind kind) {
        const char* sp = strchr(name, ' ');
        if ((sp && sp[1] == '_') || g->n >= MAXDEV) return;
        Dev& d = g->d[g->n++];
        memset(&d, 0, sizeof d);
        strlcpy(d.name, name, sizeof d.name);
        d.kind = kind; d.target = kind != SENSOR; d.max_temp = 400;
        d.col = kind == EXT ? hex(EXT_COL[ei++ % 5]) : kind == BED ? hex("009384")
              : kind == TFAN ? hex(FAN_COL[fi++ % 4]) : hex(SEN_COL[si++ % 5]);
        (void)bi;
    };
    for (int i = 0; i < k; i++) add(ext[i], EXT);
    if (ks_has("heater_bed")) add("heater_bed", BED);
    struct { const char* pre; Kind k; } groups[] = {{"heater_generic ", GENERIC}, {"temperature_fan ", TFAN},
                                                      {"temperature_sensor ", SENSOR}, {"temperature_host ", SENSOR}};
    for (auto& gr : groups) {
        int c = ks_objects(gr.pre, nm, 24);
        qsort(nm, c, 40, [](const void* a, const void* b) { return strcmp((const char*)a, (const char*)b); });
        for (int i = 0; i < c; i++) add(nm[i], gr.k);
    }
    free(nm);
}

void build_list(lv_obj_t* list) {
    int hidden = ks_cfg_int("graph_hide", 0);
    bool sel_all = !printing();
    for (int i = 0; i < g->n; i++) {
        Dev& d = g->d[i];
        d.vis = !(hidden & (1 << i));
        d.sel = sel_all && d.target;
        d.row = lv_obj_create(list);
        lv_obj_set_size(d.row, lv_pct(100), 56);
        lv_obj_set_style_radius(d.row, 0, 0);
        lv_obj_set_style_border_side(d.row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(d.row, 1, 0);
        lv_obj_set_style_border_color(d.row, KS_ACTIVE, 0);
        lv_obj_set_style_pad_all(d.row, 4, 0);
        lv_obj_clear_flag(d.row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(d.row, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_set_flex_flow(d.row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(d.row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(d.row, 6, 0);
        const char* icon = d.kind == EXT ? "extruder" : d.kind == BED ? "bed" : d.kind == GENERIC ? "heater" : d.kind == TFAN ? "fan" : "heat-up";
        ks_img(d.row, icon, 40, lv_color_white());
        char b[40];
        if (d.kind == BED) strcpy(b, "Heater Bed");
        else if (d.kind == EXT && d.name[8] == 0) strcpy(b, "Extruder");
        else pretty(d.name, b, sizeof b);
        d.nm = ks_label(d.row, b, &ks_font_22);
        lv_label_set_long_mode(d.nm, LV_LABEL_LONG_DOT);
        lv_obj_set_flex_grow(d.nm, 1);
        d.tv = ks_label(d.row, "", &ks_font_22);
        lv_obj_add_event_cb(d.row, row_click, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        lv_obj_add_event_cb(d.row, row_long, LV_EVENT_LONG_PRESSED, (void*)(intptr_t)i);
    }
}

// ---- right panel ----------------------------------------------------------------------------------------------------------
void do_cooldown(lv_event_t*) {
    if (!g || !any_selected()) return;
    for (int i = 0; i < g->n; i++) if (g->d[i].sel && g->d[i].kind != TFAN) set_target(g->d[i], 0);
}
void do_preheat(lv_event_t* e) {
    if (!g || !any_selected()) return;
    const Preset& p = PRESETS[(intptr_t)lv_event_get_user_data(e)];
    for (int i = 0; i < g->n; i++) {
        Dev& d = g->d[i];
        if (!d.sel) continue;
        int t = d.kind == EXT ? p.ext : d.kind == BED ? p.bed : -1;   // presets only define extruder and bed
        if (t >= 0 && clamp_max(d, t)) set_target(d, t);
    }
}
void do_step(lv_event_t* e) {
    if (!g || !any_selected()) return;
    int dir = (int)(intptr_t)lv_event_get_user_data(e), step = atoi(DELTAS[g->delta]);
    for (int i = 0; i < g->n; i++) {
        Dev& d = g->d[i];
        if (!d.sel) continue;
        int t = (int)ks_f(d.name, "target") + dir * step;
        if (dir > 0) clamp_max(d, t); else if (t < 0) t = 0;
        set_target(d, t);
    }
}
void build_right();
void toggle_mode(lv_event_t*) { if (g) { g->preheat = !g->preheat; build_right(); } }

void build_right() {
    lv_obj_clean(g->right);
    lv_obj_t* top = ks_card(g->right, lv_pct(100), 84);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(top, 2, 0);
    lv_obj_t* cd = ks_button_sized(top, "cool-down", 40, "Cooldown", 4, do_cooldown, nullptr);
    lv_obj_set_flex_grow(cd, 2); lv_obj_set_height(cd, lv_pct(100));
    lv_obj_t* ft = ks_button_sized(top, "fine-tune", 40, nullptr, 3, toggle_mode, nullptr);
    lv_obj_set_flex_grow(ft, 1); lv_obj_set_height(ft, lv_pct(100));
    if (g->preheat) {
        lv_obj_t* grid = ks_card(g->right, lv_pct(100), 326);
        lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_gap(grid, 2, 0);
        lv_obj_add_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
        for (int i = 0; i < (int)(sizeof PRESETS / sizeof *PRESETS); i++) {
            lv_obj_t* b = ks_button_sized(grid, nullptr, 0, PRESETS[i].name, (i % 4) + 1, do_preheat, (void*)(intptr_t)i);
            lv_obj_set_size(b, 175, 100);
        }
    } else {
        lv_obj_t* box = ks_card(g->right, lv_pct(100), 326);
        lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_style_pad_gap(box, 2, 0);
        lv_obj_t* dec = ks_button_sized(box, "decrease", 56, nullptr, 3, do_step, (void*)(intptr_t)-1);
        lv_obj_set_size(dec, 175, 170);
        lv_obj_t* inc = ks_button_sized(box, "increase", 56, nullptr, 1, do_step, (void*)(intptr_t)1);
        lv_obj_set_size(inc, 175, 170);
        lv_obj_t* l = ks_label(box, "Temperature (\xC2\xB0""C)", &ks_font_22);
        lv_obj_set_width(l, lv_pct(100));
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_t* t = ks_toggles(box, DELTAS, 4, g->delta, [](int i, void*) { if (g) g->delta = i; }, nullptr);
        lv_obj_set_width(t, lv_pct(100));
    }
}

// ---- live updates -----------------------------------------------------------------------------------------------------------
void on_state(lv_event_t*) {
    if (!g) return;
    float top = 0;
    for (int i = 0; i < g->n; i++) {
        Dev& d = g->d[i];
        float t = ks_f(d.name, "temperature", NAN), tg = ks_f(d.name, "target");
        char b[24];
        if (isnan(t)) { strcpy(b, "?"); }
        else if (d.target && tg > 0) snprintf(b, sizeof b, "%3d/%d", (int)t, (int)tg);
        else snprintf(b, sizeof b, "%3d\xC2\xB0", (int)t);
        lv_label_set_text(d.tv, b);
        if (d.ser) {
            lv_chart_set_next_value(g->chart, d.ser, isnan(t) ? LV_CHART_POINT_NONE : (lv_coord_t)lroundf(t));
            if (d.vis && !isnan(t)) top = std::max(top, std::max(t, tg));
        }
    }
    int y = std::max(100, ((int)top / 50 + 1) * 50);
    if (y != g->ytop) {
        g->ytop = y;
        lv_chart_set_range(g->chart, LV_CHART_AXIS_PRIMARY_Y, 0, y);
        char b[12]; snprintf(b, sizeof b, "%d\xC2\xB0", y);
        lv_label_set_text(g->ymax, b);
    }
}

void got_config(bool ok, JsonDocument& doc, void*) {                       // max_temp per heater, for the same clamp as Python
    if (!ok || !g) return;
    JsonObjectConst st = doc["result"]["status"]["configfile"]["settings"].as<JsonObjectConst>();
    for (int i = 0; i < g->n; i++) {
        for (JsonPairConst kv : st)
            if (!strcasecmp(kv.key().c_str(), g->d[i].name) && kv.value()["max_temp"].is<float>())
                g->d[i].max_temp = (int)kv.value()["max_temp"].as<float>();
    }
}
}  // namespace

void ks_panel_temperature(lv_obj_t* c) {
    g = new Ctx();
    g->preheat = !printing();
    collect();
    lv_obj_set_style_pad_all(c, 2, 0);
    g->root = ks_card(c, lv_pct(100), lv_pct(100));
    lv_obj_add_event_cb(g->root, [](lv_event_t*) { delete g; g = nullptr; }, LV_EVENT_DELETE, nullptr);

    lv_obj_t* left = ks_card(g->root, 352, 412);
    lv_obj_align(left, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t* list = ks_list(left);
    lv_obj_set_height(list, 196);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_row(list, 0, 0);
    build_list(list);

    g->chart = lv_chart_create(left);
    lv_obj_set_size(g->chart, 350, 212);
    lv_obj_align(g->chart, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_chart_set_type(g->chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(g->chart, POINTS);
    lv_chart_set_update_mode(g->chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_chart_set_range(g->chart, LV_CHART_AXIS_PRIMARY_Y, 0, g->ytop);
    lv_chart_set_div_line_count(g->chart, 4, 6);
    lv_obj_set_style_bg_color(g->chart, KS_BG, 0);
    lv_obj_set_style_radius(g->chart, 0, 0);
    lv_obj_set_style_border_width(g->chart, 1, 0);
    lv_obj_set_style_border_color(g->chart, KS_ACTIVE, 0);
    lv_obj_set_style_line_color(g->chart, KS_ACTIVE, LV_PART_MAIN);
    lv_obj_set_style_size(g->chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(g->chart, 2, LV_PART_ITEMS);
    for (int i = 0; i < g->n; i++) {
        g->d[i].ser = lv_chart_add_series(g->chart, g->d[i].col, LV_CHART_AXIS_PRIMARY_Y);
        if (!g->d[i].ser) continue;
        lv_chart_set_all_value(g->chart, g->d[i].ser, LV_CHART_POINT_NONE);
        if (!g->d[i].vis) lv_chart_hide_series(g->chart, g->d[i].ser, true);
        paint_row(g->d[i]);
    }
    g->ymax = ks_label(g->chart, "100\xC2\xB0", &ks_font_18);
    lv_obj_align(g->ymax, LV_ALIGN_TOP_LEFT, 2, 0);
    lv_obj_add_flag(g->ymax, LV_OBJ_FLAG_IGNORE_LAYOUT);

    g->right = ks_card(g->root, 358, 412);
    lv_obj_align(g->right, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_flex_flow(g->right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(g->right, 2, 0);
    build_right();

    ks_on_state(g->root, on_state);
    on_state(nullptr);
    ks_get_async("/printer/objects/query?configfile=settings", got_config, nullptr);
}

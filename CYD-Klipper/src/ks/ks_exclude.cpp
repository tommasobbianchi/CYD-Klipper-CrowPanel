// panels/exclude.py: "Exclude Object" - button for the object being printed, one row per remaining object, and (when Klipper
// reports polygons) the object map. Excluding the last remaining object cancels the print instead, like KlipperScreen.
#include <Arduino.h>
#include <strings.h>
#include "ks_widgets.h"
#include "ks_mr.h"
#include "ks_panels.h"

namespace {

constexpr int MAXO = 24;

struct ObjSet {
    int n = 0, nex = 0;
    char name[MAXO][40];
    bool excl[MAXO];
    char cur[40];
    bool poly = false;
};
ObjSet g_set;               // snapshot taken under the status lock (static: too big for the stack)
char g_pending[40];
uint32_t g_sig = 0;
bool g_left = false;
lv_obj_t *g_list = nullptr, *g_cur = nullptr, *g_cur_lbl = nullptr;

void snap(JsonObjectConst root, void*) {
    ObjSet& s = g_set;
    s.n = s.nex = 0; s.cur[0] = 0; s.poly = false;
    JsonObjectConst eo = root["exclude_object"];
    JsonArrayConst ex = eo["excluded_objects"];
    for (JsonObjectConst o : eo["objects"].as<JsonArrayConst>()) {
        if (s.n >= MAXO) break;
        strlcpy(s.name[s.n], o["name"] | "", 40);
        s.excl[s.n] = false;
        for (JsonVariantConst x : ex) if (!strcmp(x.as<const char*>() ? x.as<const char*>() : "", s.name[s.n])) s.excl[s.n] = true;
        if (s.excl[s.n]) s.nex++;
        if (!s.n) s.poly = o["polygon"].is<JsonArrayConst>();
        s.n++;
    }
    strlcpy(s.cur, eo["current_object"] | "", 40);
}

uint32_t signature() {
    uint32_t h = g_set.n * 1000 + g_set.nex;
    for (const char* p = g_set.cur; *p; p++) h = h * 31 + (uint8_t)*p;
    for (int i = 0; i < g_set.n; i++) h = h * 7 + g_set.excl[i];
    return h;
}

void pretty(const char* in, char* out, size_t n) {
    strlcpy(out, in, n);
    for (char* p = out; *p; p++) if (*p == '_') *p = ' ';
}

void do_cancel(void*) { ks_post("/printer/print/cancel", "{}"); }
void do_exclude(void*) { ks_gcode("EXCLUDE_OBJECT NAME=%s", g_pending); ks_toast("Excluding object"); }

void exclude(const char* name) {
    if (!name || !*name) return;
    strlcpy(g_pending, name, sizeof g_pending);
    if (g_set.nex == g_set.n - 1) {
        // the last object is never excluded: Klipper then races through the rest of the file (workaround kept from KlipperScreen)
        ks_confirm("Are you sure you wish to cancel this print?", "", "Cancel Print", do_cancel, nullptr);
        return;
    }
    char t[80]; pretty(name, t, sizeof t);
    ks_confirm("Are you sure do you want to exclude the object?", t, "Exclude", do_exclude, nullptr);
}
void obj_click(lv_event_t* e) { exclude(g_set.name[(int)(intptr_t)lv_event_get_user_data(e)]); }
void cur_click(lv_event_t*) { exclude(g_set.cur); }

void rebuild() {
    lv_obj_clean(g_list);
    for (int i = 0; i < g_set.n; i++) {
        if (g_set.excl[i]) continue;
        char t[80]; pretty(g_set.name[i], t, sizeof t);
        lv_obj_t* r = ks_row(g_list, t);
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(r, KS_ACTIVE, LV_STATE_PRESSED);
        lv_obj_add_event_cb(r, obj_click, LV_EVENT_CLICKED, (void*)(intptr_t)i);
    }
    char c[80]; pretty(g_set.cur, c, sizeof c);
    lv_label_set_text(g_cur_lbl, c);
    if (g_set.cur[0]) lv_obj_clear_state(g_cur, LV_STATE_DISABLED); else lv_obj_add_state(g_cur, LV_STATE_DISABLED);
}

void on_state(lv_event_t* e) {
    ks_with_status(snap, nullptr);
    if (g_set.n && g_set.nex == g_set.n && !g_left) { g_left = true; ks_back(); return; }   // everything excluded
    uint32_t s = signature();
    if (s != g_sig) { g_sig = s; rebuild(); lv_obj_invalidate((lv_obj_t*)lv_event_get_target(e)); }
}

// ---- object map (ks_includes/widgets/objectmap.py): polygons scaled onto the bed, y up ----------------------------------
struct MapCtx { lv_draw_ctx_t* dc; lv_area_t a; float sc; float bx, by; };

void draw_poly(JsonArrayConst poly, MapCtx& m, lv_color_t col, int w) {
    lv_draw_line_dsc_t d; lv_draw_line_dsc_init(&d);
    d.color = col; d.width = w; d.opa = LV_OPA_COVER;
    lv_point_t first{0, 0}, prev{0, 0};
    bool have = false;
    for (JsonArrayConst pt : poly) {
        lv_point_t p{(lv_coord_t)(m.a.x1 + pt[0].as<float>() * m.sc), (lv_coord_t)(m.a.y2 - pt[1].as<float>() * m.sc)};
        if (have) lv_draw_line(m.dc, &d, &prev, &p); else first = p;
        prev = p; have = true;
    }
    if (have) lv_draw_line(m.dc, &d, &prev, &first);
}
struct DrawArg { MapCtx* m; };
void draw_objs(JsonObjectConst root, void* u) {
    MapCtx& m = *((DrawArg*)u)->m;
    JsonObjectConst eo = root["exclude_object"];
    JsonArrayConst ex = eo["excluded_objects"];
    const char* cur = eo["current_object"] | "";
    for (JsonObjectConst o : eo["objects"].as<JsonArrayConst>()) {
        const char* n = o["name"] | "";
        bool excluded = false;
        for (JsonVariantConst x : ex) if (!strcmp(x.as<const char*>() ? x.as<const char*>() : "", n)) excluded = true;
        lv_color_t col = excluded ? lv_color_hex(0x555555) : !strcmp(n, cur) ? ks_color(1) : lv_color_white();
        draw_poly(o["polygon"].as<JsonArrayConst>(), m, col, !strcmp(n, cur) ? 3 : 2);
    }
}
void map_draw(lv_event_t* e) {
    lv_obj_t* o = lv_event_get_target(e);
    MapCtx m;
    m.dc = lv_event_get_draw_ctx(e);
    lv_obj_get_coords(o, &m.a);
    m.a.x1 += 4; m.a.y1 += 4; m.a.x2 -= 4; m.a.y2 -= 4;
    m.bx = ks_fi("toolhead", "axis_maximum", 0, 350);
    m.by = ks_fi("toolhead", "axis_maximum", 1, 350);
    if (m.bx < 10 || m.by < 10) return;
    float sx = (m.a.x2 - m.a.x1) / m.bx, sy = (m.a.y2 - m.a.y1) / m.by;
    m.sc = sx < sy ? sx : sy;
    DrawArg a{&m};
    ks_with_status(draw_objs, &a);
}

}   // namespace

void ks_panel_exclude(lv_obj_t* c) {
    g_sig = 0; g_left = false;
    ks_with_status(snap, nullptr);
    lv_obj_t* root = ks_card(c, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);

    g_cur = ks_button(root, "extrude", nullptr, 0, cur_click, nullptr);       // object being printed -> exclude it
    lv_obj_set_size(g_cur, lv_pct(100), 64);
    lv_obj_set_flex_flow(g_cur, LV_FLEX_FLOW_ROW);
    g_cur_lbl = ks_label(g_cur, "", &ks_font_28);
    lv_obj_t* body = ks_card(root, lv_pct(100), 340);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_ROW);
    if (g_set.poly) {
        lv_obj_t* map = ks_card(body, 330, 330);
        lv_obj_set_style_border_width(map, 2, 0);
        lv_obj_set_style_border_color(map, KS_ACTIVE, 0);
        lv_obj_add_event_cb(map, map_draw, LV_EVENT_DRAW_MAIN_END, nullptr);
        g_list = ks_list(body);
        lv_obj_set_size(g_list, 380, 340);
    } else g_list = ks_list(body);

    lv_obj_add_event_cb(root, [](lv_event_t*) { g_list = g_cur = g_cur_lbl = nullptr; }, LV_EVENT_DELETE, nullptr);
    ks_on_state(root, on_state);
    g_sig = signature();
    rebuild();
}

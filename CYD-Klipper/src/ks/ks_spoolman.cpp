// panels/spoolman.py: spool list from the Spoolman proxy (POST /server/spoolman/proxy), sort by ID / Last Used, Clear,
// Refresh, Archived switch, material filter, tap a spool to make it the active one (POST /server/spoolman/spool_id).
// Material filter is a cycling button instead of the Python's expander + combo box.
#include <Arduino.h>
#include <algorithm>
#include <string>
#include <time.h>
#include <vector>
#include "ks_widgets.h"
#include "ks_mr.h"
#include "ks_cfg.h"
#include "ks_panels.h"

namespace {

struct Spool {
    int id;
    std::string name, material, last_used;      // last_used: ISO string from Spoolman (UTC), sorts lexicographically
    uint32_t color;
    float rem_w, rem_l;                         // <0: unknown
};

enum { SORT_ID, SORT_USED };

struct Page {
    lv_obj_t *list, *mat_lbl, *err;
    std::vector<Spool> spools;
    std::vector<std::string> materials;
    int mat_sel = -1;                           // index into materials, -1 = All
    int sort = SORT_USED;
    bool desc = true;                           // Python default: Last Used, newest first
    int active = -1;
    bool busy = false;
};
Page* g_page = nullptr;

// "2026-10-24T11:22:33[.123456]" (UTC) -> local time in the 24h/12h format of KlipperScreen.conf
std::string fmt_date(const std::string& iso) {
    int y, mo, d, h, mi, s;
    if (sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6) return iso;
    struct tm t = {}; t.tm_isdst = -1;
    t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d; t.tm_hour = h; t.tm_min = mi; t.tm_sec = s;
    time_t u = mktime(&t);                      // treated as local; correct below by the zone offset
    struct tm g; gmtime_r(&u, &g); g.tm_isdst = -1;
    time_t off = u - mktime(&g);                // local - UTC for that instant
    time_t real = u + off;
    struct tm lt; localtime_r(&real, &lt);
    char b[24];
    strftime(b, sizeof b, ks_cfg_bool("24htime", true) ? "%Y-%m-%d %H:%M" : "%Y-%m-%d %I:%M %p", &lt);
    return b;
}

void rebuild(Page* pg) {
    lv_obj_clean(pg->list);
    pg->err = nullptr;
    std::vector<const Spool*> v;
    for (auto& s : pg->spools)
        if (pg->mat_sel < 0 || s.material == pg->materials[pg->mat_sel]) v.push_back(&s);
    std::sort(v.begin(), v.end(), [pg](const Spool* a, const Spool* b) {
        bool less = pg->sort == SORT_ID ? a->id < b->id : a->last_used < b->last_used;
        bool eq = pg->sort == SORT_ID ? a->id == b->id : a->last_used == b->last_used;
        if (eq) return a->id < b->id;
        return pg->desc ? !less : less;
    });
    if (v.empty()) ks_label(pg->list, "No spools", &ks_font_22);
    for (const Spool* s : v) {
        bool act = s->id == pg->active;
        char sub[200]; sub[0] = 0;
        if (!s->last_used.empty()) snprintf(sub, sizeof sub, "Last used: %s\n", fmt_date(s->last_used).c_str());
        char t[48];
        if (s->rem_w >= 0) { snprintf(t, sizeof t, "Remaining weight: %.2f g\n", s->rem_w); strlcat(sub, t, sizeof sub); }
        if (s->rem_l >= 0) { snprintf(t, sizeof t, "Remaining length: %.2f m", s->rem_l / 1000); strlcat(sub, t, sizeof sub); }
        char name[96]; snprintf(name, sizeof name, "%d  %s", s->id, s->name.c_str());
        lv_obj_t* row = ks_row(pg->list, name, sub[0] ? sub : nullptr);
        if (act) {
            lv_obj_set_style_bg_color(row, KS_ACTIVE, 0);
            lv_obj_set_style_text_font(lv_obj_get_child(lv_obj_get_child(row, 0), 0), &ks_font_28, 0);
        }
        lv_obj_t* box = ks_row_right(row, 270);
        lv_obj_t* sw = lv_obj_create(box);                                  // spool.svg colour: a round swatch
        lv_obj_set_size(sw, 36, 36);
        lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(sw, lv_color_hex(s->color), 0);
        lv_obj_set_style_border_color(sw, lv_color_white(), 0);
        lv_obj_set_style_border_width(sw, 2, 0);
        lv_obj_clear_flag(sw, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t* m = ks_label(box, s->material.c_str(), &ks_font_22);
        lv_label_set_long_mode(m, LV_LABEL_LONG_DOT);
        lv_obj_set_width(m, 110);
        if (act) ks_img(box, "complete", 40, lv_color_white());
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, [](lv_event_t* e) {
            Page* p = g_page;
            if (!p) return;
            int id = (int)(intptr_t)lv_event_get_user_data(e);
            char body[40];
            if (id == p->active) { snprintf(body, sizeof body, "{}"); p->active = -1; }      // tapping the active spool clears it
            else { snprintf(body, sizeof body, "{\"spool_id\":%d}", id); p->active = id; }
            ks_post("/server/spoolman/spool_id", body);
            rebuild(p);
        }, LV_EVENT_CLICKED, (void*)(intptr_t)s->id);
    }
}

void show_error(Page* pg, const char* text) {
    lv_obj_clean(pg->list);
    pg->err = ks_label(pg->list, text, &ks_font_22);
    lv_label_set_long_mode(pg->err, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(pg->err, lv_pct(100));
}

void on_spools(bool ok, JsonDocument& doc, void* user) {
    Page* pg = (Page*)user;
    if (pg != g_page) return;
    pg->busy = false;
    if (!ok || !doc["result"].is<JsonArrayConst>()) { show_error(pg, "Exception when trying to fetch spools"); return; }
    pg->spools.clear();
    pg->materials.clear();
    for (JsonObjectConst s : doc["result"].as<JsonArrayConst>()) {
        if (pg->spools.size() >= 100) break;
        JsonObjectConst f = s["filament"];
        Spool sp;
        sp.id = s["id"] | 0;
        std::string vendor = f["vendor"]["name"] | "";
        sp.name = (vendor.empty() ? "" : vendor + " - ") + std::string(f["name"] | "");
        sp.material = f["material"] | "";
        sp.last_used = s["last_used"] | "";
        sp.color = strtoul(f["color_hex"] | "000000", nullptr, 16);
        sp.rem_w = s["remaining_weight"].is<float>() ? s["remaining_weight"].as<float>() : -1.0f;
        sp.rem_l = s["remaining_length"].is<float>() ? s["remaining_length"].as<float>() : -1.0f;
        if (!sp.material.empty() && std::find(pg->materials.begin(), pg->materials.end(), sp.material) == pg->materials.end())
            pg->materials.push_back(sp.material);
        pg->spools.push_back(sp);
    }
    std::sort(pg->materials.begin(), pg->materials.end());
    pg->mat_sel = -1;
    lv_label_set_text(pg->mat_lbl, "Material: All");
    rebuild(pg);
}
void on_active(bool ok, JsonDocument& doc, void* user) {
    Page* pg = (Page*)user;
    if (pg != g_page || !ok) return;
    pg->active = doc["result"]["spool_id"] | -1;
    if (!pg->spools.empty()) rebuild(pg);
}

void load(Page* pg) {
    if (pg->busy) return;
    pg->busy = true;
    char body[100];
    snprintf(body, sizeof body, "{\"request_method\":\"GET\",\"path\":\"/v1/spool?allow_archived=%s\"}",
             ks_cfg_bool("spoolman_hide_archived", true) ? "false" : "true");
    ks_post_async("/server/spoolman/proxy", body, on_spools, pg);
    ks_get_async("/server/spoolman/spool_id", on_active, pg);
}

lv_obj_t* bar_button(lv_obj_t* bar, const char* text, const char* icon, int color, lv_event_cb_t cb, Page* pg, int grow) {
    lv_obj_t* b = ks_button(bar, nullptr, nullptr, color, cb, pg);
    lv_obj_set_flex_grow(b, grow);
    lv_obj_set_height(b, lv_pct(100));
    if (icon) { lv_obj_t* im = ks_img(b, icon, 40, lv_color_white()); (void)im; }
    if (text) return ks_label(b, text, &ks_font_22);
    return b;
}

void set_sort(Page* pg, int which) {
    if (pg->sort == which) pg->desc = !pg->desc; else { pg->sort = which; pg->desc = true; }      // Python: first click = descending
    rebuild(pg);
}

}  // namespace

void ks_panel_spoolman(lv_obj_t* c) {
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(c, 2, 0);
    lv_obj_set_style_pad_row(c, 2, 0);
    Page* pg = new Page();
    g_page = pg;

    lv_obj_t* bar = ks_card(c, lv_pct(100), 60);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, 4, 0);
    lv_obj_add_event_cb(bar, [](lv_event_t* e) { Page* p = (Page*)lv_event_get_user_data(e); if (g_page == p) g_page = nullptr; delete p; },
                        LV_EVENT_DELETE, pg);
    bar_button(bar, "ID", nullptr, 4, [](lv_event_t* e) { set_sort((Page*)lv_event_get_user_data(e), SORT_ID); }, pg, 1);
    bar_button(bar, "Last Used", nullptr, 3, [](lv_event_t* e) { set_sort((Page*)lv_event_get_user_data(e), SORT_USED); }, pg, 2);
    bar_button(bar, "Clear", nullptr, 2, [](lv_event_t* e) {
        Page* p = (Page*)lv_event_get_user_data(e);
        ks_post("/server/spoolman/spool_id", "{}");
        p->active = -1;
        rebuild(p);
    }, pg, 1);
    lv_obj_t* rf = ks_button_sized(bar, "refresh", 40, nullptr, 1, [](lv_event_t* e) { load((Page*)lv_event_get_user_data(e)); }, pg);
    lv_obj_set_size(rf, 72, lv_pct(100));
    ks_label(bar, "Archived", &ks_font_18);
    ks_switch(bar, !ks_cfg_bool("spoolman_hide_archived", true), [](lv_event_t* e) {       // ON = archived spools are listed
        ks_cfg_set_bool("spoolman_hide_archived", !lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
        if (g_page) load(g_page);
    }, nullptr);

    lv_obj_t* fb = ks_button(c, nullptr, nullptr, 0, [](lv_event_t* e) {                    // Material filter: tap to cycle
        Page* p = (Page*)lv_event_get_user_data(e);
        if (p->materials.empty()) return;
        p->mat_sel = p->mat_sel + 1 >= (int)p->materials.size() ? -1 : p->mat_sel + 1;
        char t[48]; snprintf(t, sizeof t, "Material: %s", p->mat_sel < 0 ? "All" : p->materials[p->mat_sel].c_str());
        lv_label_set_text(p->mat_lbl, t);
        rebuild(p);
    }, pg);
    lv_obj_set_size(fb, lv_pct(100), 48);
    pg->mat_lbl = ks_label(fb, "Material: All", &ks_font_22);

    pg->list = ks_list(c);
    lv_obj_set_flex_grow(pg->list, 1);
    lv_obj_set_height(pg->list, 0);
    ks_label(pg->list, "Loading...", &ks_font_22);
    load(pg);
}

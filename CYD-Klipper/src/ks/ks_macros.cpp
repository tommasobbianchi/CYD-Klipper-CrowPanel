// panels/gcode_macros.py (menu "gcode_macros"): list of gcode_macro entries, a run button per macro, the parameter
// prompt for macros that read `params.X|default(..)`, sort button and a settings page (show hidden '_' macros,
// per-macro "displayed" switches like [displayed_macros] in KlipperScreen.conf).
#include <Arduino.h>
#include <algorithm>
#include <map>
#include <string>
#include <vector>
#include <stdlib.h>
#include <strings.h>
#include "ks_widgets.h"
#include "ks_mr.h"
#include "ks_cfg.h"
#include "ks_panels.h"

namespace {

struct Param { std::string name, def; };
typedef std::vector<Param> ParamList;

// macro name (lower case, as in configfile) -> parameters found in its gcode. Filled once per boot, the config is big.
std::map<std::string, ParamList> g_params;
bool g_params_loaded = false, g_params_pending = false;
bool g_sort_reverse = false;

struct Panel { lv_obj_t* list; lv_timer_t* tm; };
Panel* g_panel = nullptr;                    // live panel (for the async config callback), null when closed

std::string lower(const char* s) { std::string r(s); for (auto& c : r) c = tolower((unsigned char)c); return r; }

// NVS keys are limited to 15 chars: hash the macro name.
void opt_key(const char* name, char* key, size_t n) {
    uint32_t h = 2166136261u;
    for (const char* p = name; *p; p++) { h ^= (uint8_t)tolower((uint8_t)*p); h *= 16777619u; }
    snprintf(key, n, "dm%08x", (unsigned)h);
}
bool displayed(const char* name) { char k[16]; opt_key(name, k, sizeof k); return ks_cfg_bool(k, true); }

// ---- parameters (the Python regexp, applied to every line that starts with '{' and mentions params.) ---------------
void scan_gcode(const char* gcode, ParamList& out) {
    const char* line = gcode;
    while (*line) {
        const char* eol = strchr(line, '\n');
        size_t len = eol ? (size_t)(eol - line) : strlen(line);
        std::string l(line, len);
        line = eol ? eol + 1 : line + len;
        if (l.empty() || l[0] != '{' || l.find("params.") == std::string::npos) continue;
        size_t pos = 0;
        while ((pos = l.find("params.", pos)) != std::string::npos) {
            pos += 7;
            size_t e = pos;
            while (e < l.size() && (isalnum((unsigned char)l[e]) || l[e] == '_')) e++;
            if (e == pos || l.compare(e, 9, "|default(") != 0) continue;
            std::string name = l.substr(pos, e - pos);
            size_t d0 = e + 9, d = d0;
            int depth = 1;
            while (d < l.size() && depth) { if (l[d] == '(') depth++; else if (l[d] == ')') depth--; d++; }
            std::string def = l.substr(d0, (depth ? d : d - 1) - d0);
            bool dup = false;
            for (auto& p : out) if (p.name == name) dup = true;
            if (!dup) out.push_back({name, def});
            pos = e;
        }
    }
}

void on_config(bool ok, JsonDocument& doc, void*) {
    g_params_pending = false;
    if (!ok) { g_params_loaded = true; return; }      // no hints, macros still run with their defaults
    for (JsonPairConst kv : doc["result"]["status"]["configfile"]["settings"].as<JsonObjectConst>()) {
        const char* k = kv.key().c_str();
        if (strncmp(k, "gcode_macro ", 12)) continue;
        const char* g = kv.value()["gcode"] | "";
        ParamList pl;
        scan_gcode(g, pl);
        if (!pl.empty()) g_params[k + 12] = pl;
    }
    g_params_loaded = true;
}

// ---- running ------------------------------------------------------------------------------------------------------
void run(const char* macro, const std::vector<std::string>& params) {
    std::string cmd = macro;
    char toast[128];
    for (auto& p : params) cmd += " " + p;
    snprintf(toast, sizeof toast, "%s", cmd.c_str());
    ks_toast(toast);
    ks_gcode("%s", cmd.c_str());
}
void run_default(lv_event_t* e) { run((const char*)lv_event_get_user_data(e), {}); }

// ---- parameter page (the Python shows one Gtk.Entry per parameter below the macro name) ------------------------------------
char g_cur[40];
struct PrmPage { std::vector<lv_obj_t*> entries; std::vector<std::string> names; };

void prm_run(lv_event_t* e) {
    PrmPage* pg = (PrmPage*)lv_event_get_user_data(e);
    std::vector<std::string> ps;
    for (size_t i = 0; i < pg->entries.size(); i++) {
        const char* v = lv_textarea_get_text(pg->entries[i]);
        if (v && *v) ps.push_back(pg->names[i] + "=" + v);       // empty value -> the macro's own default
    }
    ks_keyboard_hide();
    run(g_cur, ps);
}
void prm_del(lv_event_t* e) { delete (PrmPage*)lv_event_get_user_data(e); }

void params_init(lv_obj_t* c) {
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(c, 4, 0);
    lv_obj_set_style_pad_row(c, 6, 0);
    PrmPage* pg = new PrmPage;
    lv_obj_t* run_btn = ks_button_sized(c, "resume", 40, "Run", 3, prm_run, pg);   // keyboard covers the lower half: Run stays on top
    lv_obj_set_size(run_btn, 220, 72);
    lv_obj_add_event_cb(run_btn, prm_del, LV_EVENT_DELETE, pg);
    auto it = g_params.find(lower(g_cur));
    if (it == g_params.end()) return;
    for (auto& p : it->second) {
        ks_label(c, p.name.c_str(), &ks_font_22);
        lv_obj_t* ta = ks_textarea(c, p.def.c_str(), 400);          // placeholder = default, like the Python entry
        lv_obj_set_height(ta, 56);
        pg->entries.push_back(ta);
        pg->names.push_back(p.name);
    }
}
void open_params(lv_event_t* e) {
    strlcpy(g_cur, (const char*)lv_event_get_user_data(e), sizeof g_cur);
    ks_open(g_cur, params_init);
}

// ---- options page -------------------------------------------------------------------------------------------------------
void opts_init(lv_obj_t* c) {
    lv_obj_t* l = ks_list(c);
    lv_obj_t* r = ks_row(l, "Show hidden (_) macros");
    ks_switch(ks_row_right(r, 120), ks_cfg_bool("macros_hidden", false), [](lv_event_t* e) {
        ks_cfg_set_bool("macros_hidden", lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
    }, nullptr);
    static char (*names)[40]; names = (char(*)[40])malloc(40 * 128);
    int n = ks_objects("gcode_macro ", names, 128);
    std::vector<std::string> v;
    for (int i = 0; i < n; i++) v.push_back(names[i] + 12);
    free(names); names = nullptr;
    std::sort(v.begin(), v.end(), [](const std::string& a, const std::string& b) { return strcasecmp(a.c_str(), b.c_str()) < 0; });
    for (auto& m : v) {
        char* key = (char*)malloc(16);                               // freed with the switch
        opt_key(m.c_str(), key, 16);
        lv_obj_t* row = ks_row(l, m.c_str());
        lv_obj_t* sw = ks_switch(ks_row_right(row, 120), ks_cfg_bool(key, true), [](lv_event_t* e) {
            ks_cfg_set_bool((const char*)lv_event_get_user_data(e), lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED));
        }, key);
        lv_obj_add_event_cb(sw, [](lv_event_t* e) { free(lv_event_get_user_data(e)); }, LV_EVENT_DELETE, key);
    }
}

// ---- main page ----------------------------------------------------------------------------------------------------------
struct Item { std::string name; };

void fill(lv_obj_t* list) {
    lv_obj_clean(list);
    bool hidden = ks_cfg_bool("macros_hidden", false);
    char (*names)[40] = (char(*)[40])malloc(40 * 128);
    int n = names ? ks_objects("gcode_macro ", names, 128) : 0;
    std::vector<std::string> v;
    for (int i = 0; i < n; i++) {
        const char* m = names[i] + 12;
        if (m[0] == '_' && !hidden) continue;
        if (!displayed(m)) continue;
        v.push_back(m);
    }
    free(names);
    std::sort(v.begin(), v.end(), [](const std::string& a, const std::string& b) {
        int c = strcasecmp(a.c_str(), b.c_str());
        return g_sort_reverse ? c > 0 : c < 0;
    });
    if (v.empty()) { lv_obj_t* l = ks_label(list, "No macros", &ks_font_22); (void)l; }
    for (auto& m : v) {
        std::string sub;
        auto it = g_params.find(lower(m.c_str()));
        if (it != g_params.end()) { for (auto& p : it->second) sub += (sub.empty() ? "" : ", ") + p.name; }
        lv_obj_t* row = sub.empty() ? ks_row(list, m.c_str()) : ks_row(list, m.c_str(), sub.c_str());
        char* nm = (char*)malloc(40);                                  // user data for both handlers, freed with the row
        strlcpy(nm, m.c_str(), 40);
        lv_obj_add_event_cb(row, [](lv_event_t* e) { free(lv_event_get_user_data(e)); }, LV_EVENT_DELETE, nm);
        lv_obj_t* box = ks_row_right(row, 100);
        lv_obj_t* b = ks_button_sized(box, "resume", 40, nullptr, 3, run_default, nm);
        lv_obj_set_size(b, 84, 60);
        if (!sub.empty()) {                                             // tap the row to type parameters
            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(row, open_params, LV_EVENT_CLICKED, nm);
        }
    }
}

void sort_click(lv_event_t* e) {
    g_sort_reverse = !g_sort_reverse;
    lv_obj_t* lbl = (lv_obj_t*)lv_obj_get_user_data(lv_event_get_target(e));
    lv_label_set_text(lbl, g_sort_reverse ? "Name  v" : "Name  ^");
    fill(g_panel->list);
}
void open_options(lv_event_t*) { ks_open("Settings", opts_init); }

// the configfile arrives after the panel is built: add parameter hints to the rows when it does
void params_ready(lv_timer_t* t) {
    if (!g_params_loaded) return;
    Panel* p = (Panel*)t->user_data;
    p->tm = nullptr;
    fill(p->list);
    lv_timer_del(t);
}

}  // namespace

void ks_panel_macros(lv_obj_t* c) {
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(c, 2, 0);
    lv_obj_set_style_pad_row(c, 2, 0);

    Panel* p = new Panel{nullptr, nullptr};
    g_panel = p;


    lv_obj_t* bar = ks_card(c, lv_pct(100), 64);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(bar, 6, 0);
    lv_obj_add_event_cb(bar, [](lv_event_t* e) { Panel* pp = (Panel*)lv_event_get_user_data(e); if (g_panel == pp) g_panel = nullptr; if (pp->tm) lv_timer_del(pp->tm); delete pp; },
                        LV_EVENT_DELETE, p);
    lv_obj_t* sort = ks_button(bar, nullptr, nullptr, 1, sort_click, nullptr);   // sort_btn: "Name" + arrow
    lv_obj_set_flex_grow(sort, 1); lv_obj_set_height(sort, lv_pct(100));
    lv_obj_t* sl = ks_label(sort, g_sort_reverse ? "Name  v" : "Name  ^", &ks_font_22);
    lv_obj_set_user_data(sort, sl);
    lv_obj_t* adj = ks_button(bar, nullptr, nullptr, 2, open_options, nullptr);  // "Settings"
    lv_obj_set_flex_grow(adj, 1); lv_obj_set_height(adj, lv_pct(100));
    ks_label(adj, "Settings", &ks_font_22);

    p->list = ks_list(c);
    lv_obj_set_flex_grow(p->list, 1);
    lv_obj_set_height(p->list, 0);
    fill(p->list);

    if (!g_params_loaded && !g_params_pending) {
        g_params_pending = true;
        ks_get_async("/printer/objects/query?configfile=settings", on_config, nullptr);
    }
    if (!g_params_loaded) p->tm = lv_timer_create(params_ready, 500, p);
}

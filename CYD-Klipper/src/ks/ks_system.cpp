// panels/system.py + Sovol patch/klipperscreen/system.py: program list (name + version) with a restart button for the
// services Moonraker allows, host Restart / Shutdown, plus CPU / memory / temperature from /machine/proc_stats.
// Sovol's patch hides the update buttons (Full Update / Refresh / per-program Update) and shows its own firmware version
// from /boot/system.cfg; that file is not reachable through Moonraker, so the OS line (system_info) is shown instead.
// Not ported: the update flow itself (update/recover). The patch disables it on the printer as well.
#include <Arduino.h>
#include <algorithm>
#include <string>
#include <vector>
#include "ks_widgets.h"
#include "ks_mr.h"
#include "ks_panels.h"

namespace {

// Same as ALLOWED_SERVICES in the patch / moonraker machine.py
const char* const ALLOWED[] = {"crowsnest", "MoonCord", "moonraker", "moonraker-telegram-bot", "klipper", "KlipperScreen", "sonar", "webcamd"};

struct Sys {
    lv_obj_t *list, *os, *res, *klipper, *moonraker;
    lv_timer_t* timer;
    bool stats_busy = false;
    std::vector<std::string> services;       // available_services from system_info (restartable ones)
    bool alive = true;
};
Sys* g_sys = nullptr;                          // callbacks compare against this: the panel may be gone

lv_obj_t* value_label(lv_obj_t* row, int width) {
    lv_obj_t* box = ks_row_right(row, width);
    lv_obj_t* v = ks_label(box, "...", &ks_font_18);
    lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(v, width - 10);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(v, lv_color_hex(0xDDE3EA), 0);
    return v;
}

// ---- restart actions (all through ks_confirm) ---------------------------------------------------------------------
void do_service(void* name) {
    char body[96]; snprintf(body, sizeof body, "{\"service\":\"%s\"}", (const char*)name);
    ks_post("/machine/services/restart", body);
    ks_toast("Restarting service");
}
void ask_service(lv_event_t* e) {
    const char* name = (const char*)lv_event_get_user_data(e);
    char t[96]; snprintf(t, sizeof t, "Restart %s?", name);
    ks_confirm(t, "", "Restart", do_service, (void*)name);
}
void do_klipper(void*) { ks_post("/printer/restart", "{}"); ks_toast("Klipper restart sent"); }
void do_firmware(void*) { ks_post("/printer/firmware_restart", "{}"); ks_toast("Firmware restart sent"); }
void do_reboot(void*) { ks_post("/machine/reboot", "{}"); ks_toast("Rebooting host"); }
void do_shutdown(void*) { ks_post("/machine/shutdown", "{}"); ks_toast("Shutting down host"); }

// ---- data ----------------------------------------------------------------------------------------------------------
void add_program_row(Sys* s, const char* name, const std::string& text, bool restartable) {
    lv_obj_t* row = ks_row(s->list, name);
    lv_obj_t* box = ks_row_right(row, 320);
    lv_obj_t* v = ks_label(box, text.c_str(), &ks_font_18);
    lv_label_set_long_mode(v, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(v, restartable ? 230 : 310);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    if (restartable) {
        for (const char* a : ALLOWED) if (!strcmp(a, name)) {
            lv_obj_t* b = ks_button_sized(box, "refresh", 40, nullptr, 0, ask_service, (void*)a);
            lv_obj_set_size(b, 72, 56);
            break;
        }
    }
}

void on_update(bool ok, JsonDocument& doc, void*) {
    Sys* s = g_sys;
    if (!s || !ok) return;
    std::vector<std::string> names;
    for (JsonPairConst kv : doc["result"]["version_info"].as<JsonObjectConst>()) names.push_back(kv.key().c_str());
    std::sort(names.begin(), names.end());
    for (auto& n : names) {
        JsonObjectConst info = doc["result"]["version_info"][n.c_str()];
        std::string txt;
        if (n == "system") {
            int pc = info["package_count"] | 0;
            txt = pc ? std::to_string(pc) + " packages to upgrade" : "Up to date";
        } else {
            const char* v = info["version"] | "?";
            const char* r = info["remote_version"] | v;
            if (!(info["is_valid"] | true) || (info["is_dirty"] | false)) txt = std::string(v) + " (invalid)";
            else if (strcmp(v, r)) txt = std::string(v) + " -> " + r;
            else txt = v;
        }
        if (n == "klipper" && s->klipper) { lv_label_set_text(s->klipper, txt.c_str()); continue; }
        if (n == "moonraker" && s->moonraker) { lv_label_set_text(s->moonraker, txt.c_str()); continue; }
        bool restart = std::find(s->services.begin(), s->services.end(), n) != s->services.end();
        add_program_row(s, n.c_str(), txt, restart);
    }
}
void on_printer(bool ok, JsonDocument& doc, void*) {
    Sys* s = g_sys;
    if (s && ok && s->klipper) lv_label_set_text(s->klipper, doc["result"]["software_version"] | "?");
}
void on_server(bool ok, JsonDocument& doc, void*) {
    Sys* s = g_sys;
    if (s && ok && s->moonraker) lv_label_set_text(s->moonraker, doc["result"]["moonraker_version"] | "?");
}
void on_sysinfo(bool ok, JsonDocument& doc, void*) {
    Sys* s = g_sys;
    if (!s) return;
    if (ok) {
        JsonObjectConst si = doc["result"]["system_info"];
        char b[200];
        snprintf(b, sizeof b, "%s\nKernel %s\n%d x %s", si["distribution"]["name"] | "?", si["distribution"]["kernel_version"] | "?",
                 (int)(si["cpu_info"]["cpu_count"] | 0), si["cpu_info"]["processor"] | "?");
        if (s->os) lv_label_set_text(s->os, b);
        for (JsonVariantConst v : si["available_services"].as<JsonArrayConst>()) s->services.push_back(v.as<const char*>());
    }
    // chain: the rest only after the services are known, so restart buttons appear on the right rows
    ks_get_async("/printer/info", on_printer, nullptr);
    ks_get_async("/server/info", on_server, nullptr);
    ks_get_async("/machine/update/status", on_update, nullptr);
}
void on_stats(bool ok, JsonDocument& doc, void*) {
    Sys* s = g_sys;
    if (!s) return;
    s->stats_busy = false;
    if (!ok || !s->res) return;
    JsonObjectConst r = doc["result"];
    float cpu = r["system_cpu_usage"]["cpu"] | 0.0f, temp = r["cpu_temp"] | 0.0f;
    float tot = r["system_memory"]["total"] | 0.0f, used = r["system_memory"]["used"] | 0.0f;   // kB
    char b[160];
    snprintf(b, sizeof b, "CPU %.1f %%   %.1f C\nMemory %.0f / %.0f MB", cpu, temp, used / 1024, tot / 1024);
    lv_label_set_text(s->res, b);
}
void tick(lv_timer_t*) {
    if (g_sys && !g_sys->stats_busy) { g_sys->stats_busy = true; ks_get_async("/machine/proc_stats", on_stats, nullptr); }
}

lv_obj_t* action(lv_obj_t* parent, const char* icon, const char* label, int color, lv_event_cb_t cb, void* user) {
    lv_obj_t* b = ks_button_sized(parent, icon, 40, label, color, cb, user);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_height(b, lv_pct(100));
    return b;
}
#define CONFIRM(fn, title, okl) [](lv_event_t*) { ks_confirm(title, "", okl, fn, nullptr); }

}  // namespace

void ks_panel_system(lv_obj_t* c) {
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(c, 2, 0);
    lv_obj_set_style_pad_row(c, 4, 0);

    Sys* s = new Sys();
    g_sys = s;
    s->list = ks_list(c);
    lv_obj_set_flex_grow(s->list, 1);
    lv_obj_set_height(s->list, 0);
    lv_obj_add_event_cb(s->list, [](lv_event_t* e) {
        Sys* ss = (Sys*)lv_event_get_user_data(e);
        if (ss->timer) lv_timer_del(ss->timer);
        if (g_sys == ss) g_sys = nullptr;
        delete ss;
    }, LV_EVENT_DELETE, s);

    lv_obj_t* r = ks_row(s->list, "System");
    s->os = value_label(r, 320);
    r = ks_row(s->list, "Resources");
    s->res = value_label(r, 320);
    r = ks_row(s->list, "klipper");
    lv_obj_t* box = ks_row_right(r, 320);
    s->klipper = ks_label(box, "...", &ks_font_18);
    lv_label_set_long_mode(s->klipper, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s->klipper, 230);
    lv_obj_set_style_text_align(s->klipper, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_t* kb = ks_button_sized(box, "refresh", 40, nullptr, 0, ask_service, (void*)"klipper");
    lv_obj_set_size(kb, 72, 56);
    r = ks_row(s->list, "moonraker");
    box = ks_row_right(r, 320);
    s->moonraker = ks_label(box, "...", &ks_font_18);
    lv_label_set_long_mode(s->moonraker, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s->moonraker, 230);
    lv_obj_set_style_text_align(s->moonraker, LV_TEXT_ALIGN_RIGHT, 0);
    kb = ks_button_sized(box, "refresh", 40, nullptr, 0, ask_service, (void*)"moonraker");
    lv_obj_set_size(kb, 72, 56);

    // bottom bar: Klipper restart, firmware restart, host restart / shutdown
    lv_obj_t* bar = ks_card(c, lv_pct(100), 96);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(bar, 4, 0);
    action(bar, "klipper", "Klipper", 1, CONFIRM(do_klipper, "Restart Klipper?", "Restart"), nullptr);
    action(bar, "refresh", "Firmware", 2, CONFIRM(do_firmware, "Restart Klipper firmware?", "Restart"), nullptr);
    action(bar, "refresh", "Restart", 3, CONFIRM(do_reboot, "Are you sure you wish to reboot the system?", "Restart"), nullptr);
    action(bar, "shutdown", "Shutdown", 4, CONFIRM(do_shutdown, "Are you sure you wish to shutdown the system?", "Shutdown"), nullptr);

    s->timer = lv_timer_create(tick, 3000, nullptr);
    tick(nullptr);
    ks_get_async("/machine/system_info", on_sysinfo, nullptr);
}

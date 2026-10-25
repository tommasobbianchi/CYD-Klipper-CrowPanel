// panels/notifications.py: message log. KlipperScreen's own popup log (screen.notification_log) lives inside the Python
// process, so it cannot be mirrored without a hook in ks_toast (shared file). What this panel shows instead:
//  - the Klipper state message when Klipper is not ready, and the printer's display message (M117)
//  - Moonraker/Klipper announcements (GET /server/announcements/list) with a Dismiss button (POST .../dismiss)
#include <Arduino.h>
#include <string>
#include "ks_widgets.h"
#include "ks_mr.h"
#include "ks_panels.h"

namespace {

struct Page { lv_obj_t* list; };
Page* g_page = nullptr;

// colours of notifications.py (level 0 info / 2 warning / 3 error) mapped to the theme
void add_entry(lv_obj_t* list, const char* title, const char* text, lv_color_t color) {
    lv_obj_t* row = ks_row(list, title, text && *text ? text : nullptr);
    lv_obj_t* name = lv_obj_get_child(lv_obj_get_child(row, 0), 0);
    lv_obj_set_style_text_color(name, color, 0);
}

void dismiss_cb(bool, JsonDocument&, void*) {}

void on_dismiss(lv_event_t* e) {
    char* eid = (char*)lv_event_get_user_data(e);
    JsonDocument d; d["entry_id"] = eid;
    String body; serializeJson(d, body);
    ks_post_async("/server/announcements/dismiss", body.c_str(), dismiss_cb, nullptr);
    lv_obj_t* row = lv_obj_get_parent(lv_obj_get_parent(lv_event_get_target(e)));      // button -> right box -> row
    lv_obj_del_async(row);                                                                 // frees eid via the row's DELETE handler
}

void on_list(bool ok, JsonDocument& doc, void* user) {
    Page* pg = (Page*)user;
    if (pg != g_page) return;
    int shown = 0;
    if (ok) {
        for (JsonObjectConst a : doc["result"]["entries"].as<JsonArrayConst>()) {
            if (a["dismissed"] | false) continue;
            const char* prio = a["priority"] | "normal";
            lv_color_t col = !strcmp(prio, "high") ? KS_WARNING : lv_color_white();
            std::string desc = a["description"] | "";
            if (desc.size() > 150) desc = desc.substr(0, 147) + "...";
            for (auto& ch : desc) if (ch == '\n') ch = ' ';
            lv_obj_t* row = ks_row(pg->list, a["title"] | "Announcement", desc.c_str());
            lv_obj_set_style_text_color(lv_obj_get_child(lv_obj_get_child(row, 0), 0), col, 0);
            char* eid = strdup(a["entry_id"] | "");
            lv_obj_add_event_cb(row, [](lv_event_t* e) { free(lv_event_get_user_data(e)); }, LV_EVENT_DELETE, eid);
            lv_obj_t* box = ks_row_right(row, 100);
            lv_obj_t* b = ks_button_sized(box, "cancel", 40, nullptr, 2, on_dismiss, eid);
            lv_obj_set_size(b, 84, 64);
            shown++;
        }
    }
    if (!shown && !lv_obj_get_child_cnt(pg->list)) ks_label(pg->list, "Notification log empty", &ks_font_22);
}

}  // namespace

void ks_panel_notifications(lv_obj_t* c) {
    Page* pg = new Page{ks_list(c)};
    g_page = pg;
    lv_obj_add_event_cb(pg->list, [](lv_event_t* e) { Page* p = (Page*)lv_event_get_user_data(e); if (g_page == p) g_page = nullptr; delete p; },
                        LV_EVENT_DELETE, pg);

    const char* st = ks_klippy_state();
    if (strcmp(st, "ready")) {
        char msg[480]; ks_klippy_message(msg, sizeof msg);
        add_entry(pg->list, st, msg, !strcmp(st, "startup") ? KS_WARNING : lv_color_hex(0xFF6975));
    }
    char m[120];
    if (ks_str("display_status", "message", m, sizeof m) && m[0]) add_entry(pg->list, "Printer", m, lv_color_white());

    ks_get_async("/server/announcements/list", on_list, pg);
}

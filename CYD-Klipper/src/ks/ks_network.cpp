// panels/network.py. On the Panda Touch this manages the PANEL's own WiFi (not the host's): current SSID / IP / RSSI,
// a scan list, connect with a password (credentials go to global_config like the CYD wifi setup), forget network.
// Scanning and connecting run in small FreeRTOS tasks; the LVGL side polls a flag (no WiFi call blocks the UI).
#include <Arduino.h>
#include <WiFi.h>
#include <atomic>
#include <algorithm>
#include "ks_widgets.h"
#include "ks_panels.h"
#include "../conf/global_config.h"

namespace {

struct Net { char ssid[33]; int rssi; bool secure; };
const int MAX_NETS = 24;

// ---- scan task ---------------------------------------------------------------------------------------------------------
struct Scan {
    std::atomic<int> refs{2};                 // panel + task; whoever drops it to 0 frees
    std::atomic<int> state{0};                // 0 running, 1 done
    Net nets[MAX_NETS];
    int n = 0;
};
void scan_unref(Scan* s) { if (--s->refs == 0) delete s; }
void scan_task(void* p) {
    Scan* s = (Scan*)p;
    int found = WiFi.scanNetworks(false, false);          // blocks this task only
    for (int i = 0; i < found && s->n < MAX_NETS; i++) {
        String id = WiFi.SSID(i);
        if (!id.length()) continue;                        // hidden
        bool dup = false;
        for (int k = 0; k < s->n; k++) if (id == s->nets[k].ssid) { dup = true; if (WiFi.RSSI(i) > s->nets[k].rssi) s->nets[k].rssi = WiFi.RSSI(i); }
        if (dup) continue;
        Net& n = s->nets[s->n++];
        strlcpy(n.ssid, id.c_str(), sizeof n.ssid);
        n.rssi = WiFi.RSSI(i);
        n.secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    }
    WiFi.scanDelete();
    std::sort(s->nets, s->nets + s->n, [](const Net& a, const Net& b) { return a.rssi > b.rssi; });
    s->state = 1;
    scan_unref(s);
    vTaskDelete(nullptr);
}

// ---- connect task ------------------------------------------------------------------------------------------------------
struct Conn {
    std::atomic<int> refs{2};
    std::atomic<int> state{0};                // 0 running, 1 connected, 2 failed
    char ssid[33], pass[65];
};
void conn_unref(Conn* c) { if (--c->refs == 0) delete c; }
void conn_task(void* p) {
    Conn* c = (Conn*)p;
    char old_ssid[33], old_pass[65];
    strlcpy(old_ssid, global_config.wifi_SSID, sizeof old_ssid);
    strlcpy(old_pass, global_config.wifi_password, sizeof old_pass);
    WiFi.disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));
    if (c->pass[0]) WiFi.begin(c->ssid, c->pass); else WiFi.begin(c->ssid);
    for (int i = 0; i < 150 && WiFi.status() != WL_CONNECTED; i++) vTaskDelay(pdMS_TO_TICKS(100));   // 15 s
    if (WiFi.status() == WL_CONNECTED) {
        strlcpy(global_config.wifi_SSID, c->ssid, sizeof global_config.wifi_SSID);     // save only what worked
        strlcpy(global_config.wifi_password, c->pass, sizeof global_config.wifi_password);
        global_config.wifi_configured = true;
        write_global_config();
        c->state = 1;
    } else {
        WiFi.disconnect();
        if (old_ssid[0]) { if (old_pass[0]) WiFi.begin(old_ssid, old_pass); else WiFi.begin(old_ssid); }   // back to the previous network
        c->state = 2;
    }
    conn_unref(c);
    vTaskDelete(nullptr);
}

// ---- connect page ------------------------------------------------------------------------------------------------------
char g_sel_ssid[33];
bool g_sel_secure = true;

struct ConnPage {
    lv_obj_t *pass, *status, *btn;
    lv_timer_t* timer;
    Conn* conn;
};

void conn_poll(lv_timer_t* t) {
    ConnPage* pg = (ConnPage*)t->user_data;
    int st = pg->conn ? (int)pg->conn->state : 0;
    if (!st) return;
    lv_label_set_text(pg->status, st == 1 ? "Connected" : "Could not connect, previous network restored");
    lv_obj_set_style_text_color(pg->status, st == 1 ? lv_color_white() : KS_ERROR, 0);
    lv_obj_clear_state(pg->btn, LV_STATE_DISABLED);
    conn_unref(pg->conn); pg->conn = nullptr;
    lv_timer_pause(t);
}

void do_connect(lv_event_t* e) {
    ConnPage* pg = (ConnPage*)lv_event_get_user_data(e);
    if (pg->conn) return;
    Conn* c = new Conn();
    strlcpy(c->ssid, g_sel_ssid, sizeof c->ssid);
    strlcpy(c->pass, pg->pass ? lv_textarea_get_text(pg->pass) : "", sizeof c->pass);
    pg->conn = c;
    ks_keyboard_hide();
    lv_label_set_text(pg->status, "Connecting...");
    lv_obj_set_style_text_color(pg->status, lv_color_white(), 0);
    lv_obj_add_state(pg->btn, LV_STATE_DISABLED);
    lv_timer_resume(pg->timer);
    if (xTaskCreate(conn_task, "ks_wifi_conn", 4096, c, 1, nullptr) != pdPASS) {
        c->state = 2; conn_unref(c);                       // task never ran: drop its reference ourselves
    }
}

void conn_init(lv_obj_t* c) {
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(c, 6, 0);
    lv_obj_set_style_pad_row(c, 10, 0);
    ConnPage* pg = new ConnPage{nullptr, nullptr, nullptr, nullptr, nullptr};
    lv_obj_t* name = ks_label(c, g_sel_ssid, &ks_font_28);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(name, lv_pct(100));
    lv_obj_t* row = ks_card(c, lv_pct(100), 64);               // password entry above the keyboard area
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 6, 0);
    if (g_sel_secure) {
        pg->pass = ks_textarea(row, "Password", 480, true);
        lv_textarea_set_max_length(pg->pass, 64);
        lv_obj_set_height(pg->pass, 60);
        lv_obj_add_event_cb(pg->pass, do_connect, LV_EVENT_READY, pg);
    } else {
        ks_label(row, "Open network", &ks_font_22);
    }
    pg->btn = ks_button_sized(row, "complete", 40, "Connect", 3, do_connect, pg);
    lv_obj_set_size(pg->btn, 180, 64);
    pg->status = ks_label(c, "", &ks_font_22);
    lv_label_set_long_mode(pg->status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(pg->status, lv_pct(100));
    pg->timer = lv_timer_create(conn_poll, 300, pg);
    lv_timer_pause(pg->timer);
    lv_obj_add_event_cb(row, [](lv_event_t* e) {
        ConnPage* p = (ConnPage*)lv_event_get_user_data(e);
        lv_timer_del(p->timer);
        if (p->conn) conn_unref(p->conn);                  // task keeps running and finishes (saves / restores) on its own
        delete p;
    }, LV_EVENT_DELETE, pg);
}

// ---- main page ---------------------------------------------------------------------------------------------------------
struct Page {
    lv_obj_t *info, *list, *scan_btn;
    lv_timer_t *timer;
    Scan* scan;
    bool listed;
};

void fill(Page* pg, Scan* s) {
    lv_obj_clean(pg->list);
    String cur = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String();
    if (!s->n) ks_label(pg->list, "No networks found", &ks_font_22);
    for (int i = 0; i < s->n; i++) {
        Net& n = s->nets[i];
        bool connected = cur == n.ssid;
        char sub[48]; snprintf(sub, sizeof sub, "%d dBm  %s%s", n.rssi, n.secure ? "secured" : "open", connected ? "  (connected)" : "");
        lv_obj_t* row = ks_row(pg->list, n.ssid, sub);
        lv_obj_set_height(row, 84);
        if (connected) lv_obj_set_style_bg_color(row, KS_ACTIVE, 0);
        lv_obj_t* box = ks_row_right(row, 100);
        ks_img(box, connected ? "complete" : "network", 40, lv_color_white());
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        Net* copy = new Net(n);                            // owned by the row
        lv_obj_add_event_cb(row, [](lv_event_t* e) { delete (Net*)lv_event_get_user_data(e); }, LV_EVENT_DELETE, copy);
        lv_obj_add_event_cb(row, [](lv_event_t* e) {
            Net* nn = (Net*)lv_event_get_user_data(e);
            strlcpy(g_sel_ssid, nn->ssid, sizeof g_sel_ssid);
            g_sel_secure = nn->secure;
            ks_open("Connect", conn_init);
        }, LV_EVENT_CLICKED, copy);
    }
}

void start_scan(Page* pg) {
    if (pg->scan) return;
    Scan* s = new Scan();
    pg->scan = s;
    lv_obj_add_state(pg->scan_btn, LV_STATE_DISABLED);
    lv_obj_clean(pg->list);
    ks_label(pg->list, "Scanning...", &ks_font_22);
    if (xTaskCreate(scan_task, "ks_wifi_scan", 4096, s, 1, nullptr) != pdPASS) { s->state = 1; scan_unref(s); }
}

void update_info(Page* pg) {
    char b[200];
    if (WiFi.status() == WL_CONNECTED)
        snprintf(b, sizeof b, "SSID: %s\nIP: %s    Signal: %d dBm", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str(), WiFi.RSSI());
    else snprintf(b, sizeof b, "Not connected\nSaved network: %s", global_config.wifi_SSID[0] ? global_config.wifi_SSID : "none");
    lv_label_set_text(pg->info, b);
}

void page_tick(lv_timer_t* t) {
    Page* pg = (Page*)t->user_data;
    update_info(pg);
    if (pg->scan && pg->scan->state == 1) {
        fill(pg, pg->scan);
        scan_unref(pg->scan); pg->scan = nullptr;
        lv_obj_clear_state(pg->scan_btn, LV_STATE_DISABLED);
    }
}

void do_forget(void*) {
    global_config.wifi_configured = false;
    global_config.wifi_SSID[0] = 0;
    global_config.wifi_password[0] = 0;
    write_global_config();
    ks_toast("Network forgotten, restarting...");
    lv_timer_create([](lv_timer_t*) { WiFi.disconnect(true, true); ESP.restart(); }, 1500, nullptr);   // boot shows the WiFi setup
}

}  // namespace

void ks_panel_network(lv_obj_t* c) {
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(c, 2, 0);
    lv_obj_set_style_pad_row(c, 2, 0);

    Page* pg = new Page{nullptr, nullptr, nullptr, nullptr, nullptr, false};
    lv_obj_t* bar = ks_card(c, lv_pct(100), 84);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, 6, 0);
    pg->info = ks_label(bar, "", &ks_font_22);
    lv_obj_set_flex_grow(pg->info, 1);
    lv_label_set_long_mode(pg->info, LV_LABEL_LONG_DOT);
    pg->scan_btn = ks_button_sized(bar, "refresh", 40, nullptr, 1, [](lv_event_t* e) { start_scan((Page*)lv_event_get_user_data(e)); }, pg);
    lv_obj_set_size(pg->scan_btn, 84, 76);
    lv_obj_t* forget = ks_button_sized(bar, "delete", 40, nullptr, 2, [](lv_event_t*) {
        ks_confirm("Forget the saved network?", "The panel disconnects and restarts to ask for a new WiFi.", "Forget", do_forget, nullptr);
    }, nullptr);
    lv_obj_set_size(forget, 84, 76);
    lv_obj_add_event_cb(bar, [](lv_event_t* e) {
        Page* p = (Page*)lv_event_get_user_data(e);
        lv_timer_del(p->timer);
        if (p->scan) scan_unref(p->scan);
        delete p;
    }, LV_EVENT_DELETE, pg);

    pg->list = ks_list(c);
    lv_obj_set_flex_grow(pg->list, 1);
    lv_obj_set_height(pg->list, 0);
    pg->timer = lv_timer_create(page_tick, 500, pg);
    update_info(pg);
    start_scan(pg);
}

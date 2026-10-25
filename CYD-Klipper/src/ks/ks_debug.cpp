// Development aid: GET http://<panel>:8081/shot returns the current LVGL screen as raw RGB565 (800x480, little endian),
// so layouts can be checked from a script instead of by eye. Served from loop() (LVGL thread), so no locking needed.
#include <WiFi.h>
#include <WebServer.h>
#include <esp_heap_caps.h>
#include "lvgl.h"
#include "ks_ui.h"
#include "ks_mr.h"
#include "ks_panels.h"

uint32_t pt_vsync_count();
uint32_t pt_pclk_khz();
static WebServer* g_srv;
static uint32_t g_wifi_off_until = 0;                 // diagnostics: WiFi radio held off until this millis()
bool ks_wifi_held() { return g_wifi_off_until && (int32_t)(millis() - g_wifi_off_until) < 0; }

// /info: pixel clock, scanned frames (two samples 1 s apart give the real refresh rate), heap, reset reason
static void handle_info() {
    uint32_t v0 = pt_vsync_count(); uint32_t t0 = millis();
    delay(1000);
    uint32_t v1 = pt_vsync_count(); uint32_t t1 = millis();
    char b[256];
    snprintf(b, sizeof b, "{\"pclk_khz\":%u,\"measured_hz\":%.2f,\"uptime_s\":%u,\"free_heap\":%u,\"free_psram\":%u,\"reset\":%d}",
             (unsigned)pt_pclk_khz(), (v1 - v0) * 1000.0 / (t1 - t0), (unsigned)(millis() / 1000), (unsigned)ESP.getFreeHeap(),
             (unsigned)ESP.getFreePsram(), (int)esp_reset_reason());
    g_srv->send(200, "application/json", b);
}

// /open?p=<name> shows a panel (for screenshot-driven checks); /home and /back navigate.
struct PanelName { const char* name; ks_panel_init fn; };
static const PanelName PANELS[] = {
    {"move", ks_panel_move}, {"temperature", ks_panel_temperature}, {"extrude", ks_panel_extrude}, {"print", ks_panel_print},
    {"bed_level", ks_panel_bed_level}, {"bed_mesh", ks_panel_bed_mesh}, {"zcalibrate", ks_panel_zcalibrate},
    {"limits", ks_panel_limits}, {"retraction", ks_panel_retraction}, {"fan", ks_panel_fan}, {"led", ks_panel_led},
    {"macros", ks_panel_macros}, {"pins", ks_panel_pins}, {"power", ks_panel_power}, {"camera", ks_panel_camera},
    {"console", ks_panel_console}, {"system", ks_panel_system}, {"input_shaper", ks_panel_input_shaper},
    {"settings", ks_panel_settings}, {"network", ks_panel_network}, {"notifications", ks_panel_notifications},
    {"spoolman", ks_panel_spoolman}, {"job_status", ks_panel_job_status},
};
static void handle_open() {
    String n = g_srv->arg("p");
    for (const PanelName& p : PANELS)
        if (n == p.name) { ks_open(p.name, p.fn); g_srv->send(200, "text/plain", "ok"); return; }
    if (n == "more") { ks_home(); g_srv->send(200, "text/plain", "use /home then tap More"); return; }
    g_srv->send(404, "text/plain", "unknown panel");
}

static void handle_shot() {
    lv_obj_t* scr = lv_scr_act();
    size_t sz = lv_snapshot_buf_size_needed(scr, LV_IMG_CF_TRUE_COLOR);
    uint8_t* buf = (uint8_t*)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
    lv_img_dsc_t dsc;
    if (!buf || lv_snapshot_take_to_buf(scr, LV_IMG_CF_TRUE_COLOR, &dsc, buf, sz) != LV_RES_OK) {
        g_srv->send(500, "text/plain", "snapshot failed");
        heap_caps_free(buf);
        return;
    }
    g_srv->setContentLength(sz);
    g_srv->sendHeader("X-Width", String(dsc.header.w));
    g_srv->sendHeader("X-Height", String(dsc.header.h));
    g_srv->send(200, "application/octet-stream", "");
    WiFiClient c = g_srv->client();
    for (size_t off = 0; off < sz; off += 1460) c.write(buf + off, min((size_t)1460, sz - off));
    heap_caps_free(buf);
}

// WiFi supervision for the KlipperScreen UI. CYD's wifi_ok() takes request-thread semaphores that only exist in the stock UI,
// so in KS mode a dropped connection would block loop() forever; this one never blocks.
void ks_wifi_supervise();
#include "../conf/global_config.h"
void ks_wifi_supervise() {
    static uint32_t down_since = 0, last_try = 0;
    if (ks_wifi_held() || !global_config.wifi_configured) return;
    if (WiFi.status() == WL_CONNECTED) { down_since = 0; return; }
    uint32_t now = millis();
    if (!down_since) down_since = now;
    if (now - down_since > 8000 && now - last_try > 15000) {      // give auto-reconnect 8 s, then retry every 15 s
        last_try = now;
        WiFi.disconnect();
        if (global_config.wifi_password[0]) WiFi.begin(global_config.wifi_SSID, global_config.wifi_password);
        else WiFi.begin(global_config.wifi_SSID);
    }
}

void ks_debug_loop() {
    if (g_wifi_off_until && !ks_wifi_held()) {         // hold expired: bring the radio back
        g_wifi_off_until = 0;
        WiFi.mode(WIFI_STA);          // ks_wifi_supervise() reconnects with the saved credentials
    }
    if (ks_wifi_held()) return;                        // no server while the radio is off
    if (!g_srv) {
        g_srv = new WebServer(8081);
        g_srv->on("/shot", handle_shot);
        g_srv->on("/open", handle_open);
        g_srv->on("/info", handle_info);
        g_srv->on("/wifi_off", []() {                  // /wifi_off?s=120 : radio off for s seconds, then it comes back by itself
            int sec = constrain(g_srv->arg("s").toInt(), 5, 600);
            g_srv->send(200, "text/plain", String("wifi off for ") + sec + " s");
            delay(300);
            g_wifi_off_until = millis() + sec * 1000UL;
            WiFi.disconnect(true, false);
            WiFi.mode(WIFI_OFF);
        });
        g_srv->on("/poll", []() { ks_poll_enable(g_srv->arg("on") != "0"); g_srv->send(200, "text/plain", g_srv->arg("on") != "0" ? "poll on" : "poll off"); });
        g_srv->on("/home", []() { ks_home(); g_srv->send(200, "text/plain", "ok"); });
        g_srv->on("/back", []() { ks_back(); g_srv->send(200, "text/plain", "ok"); });
        g_srv->begin();
    }
    g_srv->handleClient();
}

// panels/camera.py: list of Moonraker webcams (GET /server/webcams/list) and a live view. The Python plays the MJPEG stream
// with mpv; here the snapshot_url is fetched about once a second and decoded with ESP-IDF's esp_jpeg (tjpgd) at 1/2 scale
// into RGB565 (PSRAM). Download + decode run in a small FreeRTOS task, the LVGL side polls for the finished frame.
// Webcam URLs are served by the host's web server (port 80), not by Moonraker, so this file uses HTTPClient directly.
// Rotation 0/90/180/270 is applied; flip_horizontal / flip_vertical are not (LVGL images cannot be mirrored).
#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <atomic>
#include <vector>
#include <esp_heap_caps.h>
#include "jpeg_decoder.h"
#include "ks_widgets.h"
#include "ks_mr.h"
#include "ks_panels.h"
#include "../conf/global_config.h"

namespace {

struct Cam { char name[40]; char snapshot[200]; int rot; };
Cam g_cam;                                           // webcam chosen in the list, read by the viewer

// ---- viewer ---------------------------------------------------------------------------------------------------------
struct Frame { uint16_t* px; int w, h; };
struct View {
    std::atomic<int> refs{2};                        // viewer panel + task
    std::atomic<bool> run{true};
    std::atomic<int> errors{0};
    std::atomic<Frame*> pending{nullptr};            // newest decoded frame, taken by the LVGL timer
    char url[260];
    lv_obj_t *img, *status;
    lv_timer_t* timer;
    Frame* shown = nullptr;
    lv_img_dsc_t dsc;
    int rot;
};
void view_unref(View* v) { if (--v->refs == 0) delete v; }
void frame_free(Frame* f) { if (f) { heap_caps_free(f->px); delete f; } }

// GET url into a PSRAM buffer (max 600 KB). Returns size or 0.
size_t download(const char* url, uint8_t** out) {
    HTTPClient c;
    c.setReuse(false);
    c.useHTTP10(true);                               // nginx answers chunked otherwise, and the body is read raw below
    c.setConnectTimeout(4000);
    c.setTimeout(5000);
    if (!c.begin(url)) return 0;
    size_t got = 0;
    if (c.GET() == 200) {
        const size_t CAP = 600 * 1024;
        int sz = c.getSize();
        size_t cap = sz > 0 ? (size_t)sz : CAP;
        if (cap <= CAP) {
            uint8_t* buf = (uint8_t*)heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
            if (buf) {
                WiFiClient* st = c.getStreamPtr();
                uint32_t last = millis();
                while (got < cap && millis() - last < 4000 && (c.connected() || st->available())) {
                    int n = st->readBytes(buf + got, cap - got < 4096 ? cap - got : 4096);
                    if (n > 0) { got += n; last = millis(); } else vTaskDelay(pdMS_TO_TICKS(5));
                    if (sz > 0 && got == cap) break;
                }
                if (sz > 0 && got != cap) got = 0;               // truncated
                if (got) *out = buf; else heap_caps_free(buf);
            }
        }
    }
    c.end();
    return got;
}

Frame* decode(uint8_t* jpg, size_t len) {
    esp_jpeg_image_cfg_t cfg = {};
    cfg.indata = jpg; cfg.indata_size = (uint32_t)len;
    cfg.out_format = JPEG_IMAGE_FORMAT_RGB565;
    cfg.out_scale = JPEG_IMAGE_SCALE_1_2;
    esp_jpeg_image_output_t info;
    if (esp_jpeg_get_image_info(&cfg, &info) != ESP_OK) return nullptr;
    uint16_t* px = (uint16_t*)heap_caps_malloc(info.output_len, MALLOC_CAP_SPIRAM);
    if (!px) return nullptr;
    cfg.outbuf = (uint8_t*)px; cfg.outbuf_size = (uint32_t)info.output_len;
    if (esp_jpeg_decode(&cfg, &info) != ESP_OK) { heap_caps_free(px); return nullptr; }
    return new Frame{px, info.width, info.height};
}

void view_task(void* p) {
    View* v = (View*)p;
    while (v->run) {
        uint32_t t0 = millis();
        uint8_t* jpg = nullptr;
        size_t n = download(v->url, &jpg);
        Frame* f = n ? decode(jpg, n) : nullptr;
        if (jpg) heap_caps_free(jpg);
        if (f) {
            v->errors = 0;
            frame_free(v->pending.exchange(f));
        } else v->errors++;
        uint32_t spent = millis() - t0;
        for (uint32_t w = spent < 1000 ? 1000 - spent : 100; w > 0 && v->run; w -= (w > 100 ? 100 : w)) vTaskDelay(pdMS_TO_TICKS(w > 100 ? 100 : w));
    }
    frame_free(v->pending.exchange(nullptr));
    view_unref(v);
    vTaskDelete(nullptr);
}

void view_poll(lv_timer_t* t) {
    View* v = (View*)t->user_data;
    Frame* f = v->pending.exchange(nullptr);
    if (f) {
        v->dsc = {};
        v->dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
        v->dsc.header.w = f->w; v->dsc.header.h = f->h;
        v->dsc.data_size = f->w * f->h * 2;
        v->dsc.data = (const uint8_t*)f->px;
        lv_img_set_src(v->img, &v->dsc);                          // the old buffer is released only after the new source is set
        lv_img_set_pivot(v->img, f->w / 2, f->h / 2);
        lv_img_set_angle(v->img, v->rot * 10);
        bool side = v->rot == 90 || v->rot == 270;
        int fw = side ? f->h : f->w, fh = side ? f->w : f->h;
        int z = 256 * 716 / fw, zh = 256 * 416 / fh;              // fit the 716 x 416 content area
        lv_img_set_zoom(v->img, z < zh ? z : zh);
        lv_obj_center(v->img);
        frame_free(v->shown);
        v->shown = f;
        lv_obj_add_flag(v->status, LV_OBJ_FLAG_HIDDEN);
    } else if (v->errors >= 2 && !v->shown) {
        lv_label_set_text(v->status, "Camera not reachable");
        lv_obj_clear_flag(v->status, LV_OBJ_FLAG_HIDDEN);
    }
}

void view_init(lv_obj_t* c) {
    View* v = new View();
    v->rot = g_cam.rot;
    if (!strncmp(g_cam.snapshot, "http", 4)) strlcpy(v->url, g_cam.snapshot, sizeof v->url);
    else snprintf(v->url, sizeof v->url, "http://%s%s%s", global_config.printer_config[global_config.printer_index].printer_host,
                  g_cam.snapshot[0] == '/' ? "" : "/", g_cam.snapshot);
    v->img = lv_img_create(c);
    lv_obj_center(v->img);
    v->status = ks_label(c, "Loading...", &ks_font_28);
    lv_obj_center(v->status);
    v->timer = lv_timer_create(view_poll, 250, v);
    lv_obj_add_event_cb(v->img, [](lv_event_t* e) {
        View* vv = (View*)lv_event_get_user_data(e);
        lv_timer_del(vv->timer);
        vv->run = false;                                           // task notices within 100 ms, then frees its frames
        frame_free(vv->shown); vv->shown = nullptr;
        view_unref(vv);
    }, LV_EVENT_DELETE, v);
    if (xTaskCreate(view_task, "ks_cam", 8192, v, 1, nullptr) != pdPASS) { v->errors = 5; view_unref(v); }
}

void open_view(const char* name, const char* snapshot, int rot) {
    strlcpy(g_cam.name, name, sizeof g_cam.name);
    strlcpy(g_cam.snapshot, snapshot, sizeof g_cam.snapshot);
    g_cam.rot = rot;
    ks_open(g_cam.name, view_init);
}

// ---- list -----------------------------------------------------------------------------------------------------------------
struct Page { lv_obj_t* list; };
Page* g_page = nullptr;

struct Item { char name[40], snap[200]; int rot; };

void on_webcams(bool ok, JsonDocument& doc, void* user) {
    Page* pg = (Page*)user;
    if (pg != g_page) return;
    std::vector<Item> cams;
    if (ok) for (JsonObjectConst w : doc["result"]["webcams"].as<JsonArrayConst>()) {
        if (!(w["enabled"] | true)) continue;
        const char* snap = w["snapshot_url"] | "";
        if (!*snap) continue;
        Item it;
        strlcpy(it.name, w["name"] | "camera", sizeof it.name);
        strlcpy(it.snap, snap, sizeof it.snap);
        it.rot = ((int)(w["rotation"] | 0) % 360 + 360) % 360;
        cams.push_back(it);
    }
    if (cams.size() == 1) {                                     // activate(): a single camera opens straight away
        ks_back();                                              // replace the list by the viewer, so Back returns to the menu
        open_view(cams[0].name, cams[0].snap, cams[0].rot);
        return;
    }
    if (cams.empty()) { ks_label(pg->list, ok ? "No webcams configured" : "Could not load webcams", &ks_font_22); return; }
    int i = 0;
    for (auto& cam : cams) {
        Item* copy = new Item(cam);
        lv_obj_t* b = ks_button_sized(pg->list, "camera", 56, cam.name, i % 4 + 1, [](lv_event_t* e) {
            Item* it = (Item*)lv_event_get_user_data(e);
            open_view(it->name, it->snap, it->rot);
        }, copy);
        lv_obj_set_size(b, lv_pct(100), 96);
        lv_obj_add_event_cb(b, [](lv_event_t* e) { delete (Item*)lv_event_get_user_data(e); }, LV_EVENT_DELETE, copy);
        i++;
    }
}

}  // namespace

void ks_panel_camera(lv_obj_t* c) {
    Page* pg = new Page{ks_list(c)};
    g_page = pg;
    lv_obj_add_event_cb(pg->list, [](lv_event_t* e) { Page* p = (Page*)lv_event_get_user_data(e); if (g_page == p) g_page = nullptr; delete p; },
                        LV_EVENT_DELETE, pg);
    ks_get_async("/server/webcams/list", on_webcams, pg);
}

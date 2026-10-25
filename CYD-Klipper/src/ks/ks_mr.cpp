#include "ks_mr.h"
#include <HTTPClient.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include "../conf/global_config.h"

// JSON documents use the default allocator: small blocks stay in internal RAM, which keeps PSRAM bandwidth
// free for the RGB panel DMA (poll parsing in PSRAM caused visible display glitches).

// Large one-off responses (configfile settings, file lists) get their JSON pool in PSRAM; the 1 Hz status document stays in
// internal RAM so the poll never touches the PSRAM bus the display scans from.
struct PsramAllocator : ArduinoJson::Allocator {
    void* allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM); }
    void deallocate(void* p) override { heap_caps_free(p); }
    void* reallocate(void* p, size_t n) override { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM); }
};
static PsramAllocator g_psram_alloc;

static JsonDocument g_status;                      // merged printer object status            // merged printer object status
static SemaphoreHandle_t g_lock;                    // guards g_status + g_flags
static bool g_connected = false;
static char g_klippy[16] = "disconnected";
static char g_klippy_msg[480] = "";
static volatile bool g_dirty = false;
static char g_query[900] = "";                      // objects=... part of the poll URL

// ---- request queue (LVGL thread -> worker) and result queue (worker -> LVGL thread) --------
struct Req {
    char* path; char* body; bool post;
    ks_get_cb cb; void* user;
    ks_bytes_cb bcb;                     // binary download when set
};
struct Res {
    JsonDocument* doc; bool ok; ks_get_cb cb; void* user;
    uint8_t* data; size_t len; ks_bytes_cb bcb;
};
static QueueHandle_t g_req_q, g_res_q;

static String base_url(const char* path) {
    PrinterConfiguration* pc = &global_config.printer_config[global_config.printer_index];
    return "http://" + String(pc->printer_host) + ":" + String(pc->klipper_port) + path;
}

static void begin(HTTPClient& c, const char* path, int timeout_ms) {
    c.setReuse(false);
    c.setConnectTimeout(timeout_ms);
    c.setTimeout(timeout_ms);
    c.begin(base_url(path));
    PrinterConfiguration* pc = &global_config.printer_config[global_config.printer_index];
    if (pc->printer_auth[0]) c.addHeader("X-Api-Key", pc->printer_auth);
}

// GET/POST and parse into *out (PSRAM). Returns true on HTTP 2xx + valid JSON.
static bool http_json(const char* path, const char* body, bool post, JsonDocument* out, int timeout_ms = 4000) {
    if (WiFi.status() != WL_CONNECTED) return false;
    HTTPClient c;
    begin(c, path, timeout_ms);
    int code;
    if (post) { c.addHeader("Content-Type", "application/json"); code = c.POST(body ? body : "{}"); }
    else code = c.GET();
    bool ok = false;
    if (code >= 200 && code < 300) {
        if (out) {
            String s = c.getString();
            ok = !deserializeJson(*out, s);
        } else ok = true;
    }
    c.end();
    return ok;
}

static void set_klippy(const char* st, const char* msg) {
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (strcmp(g_klippy, st)) g_dirty = true;
    strlcpy(g_klippy, st, sizeof g_klippy);
    strlcpy(g_klippy_msg, msg ? msg : "", sizeof g_klippy_msg);
    xSemaphoreGive(g_lock);
}

// Everything except macros/noise is polled, like KlipperScreen's subscribe-to-all.
static bool build_query() {
    JsonDocument d;
    if (!http_json("/printer/objects/list", nullptr, false, &d)) return false;
    String q = "/printer/objects/query?";
    bool first = true;
    for (JsonVariantConst o : d["result"]["objects"].as<JsonArrayConst>()) {
        const char* n = o.as<const char*>();
        if (!n || !strncmp(n, "gcode_macro", 11) || !strcmp(n, "configfile") || !strcmp(n, "mcu") ||
            !strncmp(n, "mcu ", 4) || !strcmp(n, "gcode") || !strncmp(n, "tmc", 3))
            continue;
        String enc = n;
        enc.replace(" ", "%20");
        if (!first) q += "&";
        q += enc; first = false;
        if (q.length() > sizeof(g_query) - 40) break;
    }
    // macro NAMES are still needed by the Macros panel: keep them in the document
    xSemaphoreTake(g_lock, portMAX_DELAY);
    strlcpy(g_query, q.c_str(), sizeof g_query);
    JsonArray macros = g_status["__macros"].to<JsonArray>();
    for (JsonVariantConst o : d["result"]["objects"].as<JsonArrayConst>()) {
        const char* n = o.as<const char*>();
        if (n && !strncmp(n, "gcode_macro ", 12)) macros.add(String(n + 12));
    }
    JsonArray names = g_status["__objects"].to<JsonArray>();
    for (JsonVariantConst o : d["result"]["objects"].as<JsonArrayConst>()) names.add(o.as<const char*>());
    xSemaphoreGive(g_lock);
    return true;
}

static volatile bool g_poll_on = true;
void ks_poll_enable(bool on) { g_poll_on = on; }

static bool poll_once() {
    if (!g_poll_on) return true;
    JsonDocument d;
    if (!http_json(g_query, nullptr, false, &d)) {
        if (g_connected) { g_connected = false; g_dirty = true; }
        return false;
    }
    JsonObjectConst st = d["result"]["status"];
    xSemaphoreTake(g_lock, portMAX_DELAY);
    for (JsonPairConst kv : st) g_status[kv.key()] = kv.value();   // deep copy per object
    g_dirty = true;
    xSemaphoreGive(g_lock);
    return true;
}

static void service_requests() {
    Req r;
    while (xQueueReceive(g_req_q, &r, 0) == pdTRUE) {
        if (r.bcb) {                                              // binary download into PSRAM
            uint8_t* data = nullptr; size_t len = 0; bool ok = false;
            if (WiFi.status() == WL_CONNECTED) {
                HTTPClient c;
                begin(c, r.path, 10000);
                if (c.GET() == 200) {
                    int sz = c.getSize();
                    if (sz > 0 && sz <= 3 * 1024 * 1024) {
                        data = (uint8_t*)heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
                        if (data) {
                            WiFiClient* st = c.getStreamPtr();
                            size_t got = 0; uint32_t last = millis();
                            while (got < (size_t)sz && millis() - last < 8000) {
                                int n = st->readBytes(data + got, min((size_t)1460 * 4, (size_t)sz - got));
                                if (n > 0) { got += n; last = millis(); } else delay(2);
                            }
                            ok = got == (size_t)sz; len = got;
                            if (!ok) { heap_caps_free(data); data = nullptr; }
                        }
                    }
                }
                c.end();
            }
            Res res{nullptr, ok, nullptr, r.user, data, len, r.bcb};
            if (xQueueSend(g_res_q, &res, pdMS_TO_TICKS(50)) != pdTRUE && data) heap_caps_free(data);
            free(r.path); free(r.body);
            continue;
        }
        JsonDocument* doc = r.cb ? new JsonDocument(&g_psram_alloc) : nullptr;
        bool ok = http_json(r.path, r.body, r.post, doc, 8000);
        if (r.cb) { Res res{doc, ok, r.cb, r.user, nullptr, 0, nullptr}; if (xQueueSend(g_res_q, &res, pdMS_TO_TICKS(50)) != pdTRUE) delete doc; }
        free(r.path); free(r.body);
    }
}

static void worker(void*) {
    uint32_t last_poll = 0;
    for (;;) {
        service_requests();
        if (WiFi.status() != WL_CONNECTED) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }

        if (!g_connected) {
            JsonDocument info;
            if (http_json("/printer/info", nullptr, false, &info)) {
                const char* st = info["result"]["state"] | "error";
                set_klippy(st, info["result"]["state_message"] | "");
                if (!strcmp(st, "ready") && build_query() && poll_once()) { g_connected = true; g_dirty = true; }  // first status must be in before the UI is built
            } else set_klippy("disconnected", "Moonraker unreachable");
            vTaskDelay(pdMS_TO_TICKS(g_connected ? 10 : 1500));
            continue;
        }
        if (millis() - last_poll >= 1000) { last_poll = millis(); poll_once(); }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void ks_mr_start() {
    static bool started = false;
    if (started) return;
    started = true;
    g_lock = xSemaphoreCreateMutex();
    g_req_q = xQueueCreate(24, sizeof(Req));
    g_res_q = xQueueCreate(8, sizeof(Res));
    xTaskCreatePinnedToCore(worker, "ks_mr", 12288, nullptr, 1, nullptr, 0);
}

// ---- LVGL-thread delivery ------------------------------------------------------------------
static void tick_cb(lv_timer_t*) {
    Res r;
    while (xQueueReceive(g_res_q, &r, 0) == pdTRUE) {
        if (r.bcb) { r.bcb(r.ok, r.data, r.len, r.user); continue; }
        if (r.cb) {
            if (r.doc) r.cb(r.ok, *r.doc, r.user);
        }
        delete r.doc;
    }
    if (g_dirty) { g_dirty = false; lv_msg_send(KS_MSG_STATE, nullptr); }
}
void ks_mr_tick_init() { lv_timer_create(tick_cb, 50, nullptr); }

// ---- status accessors ----------------------------------------------------------------------
void ks_retry() { g_connected = false; g_dirty = true; }
bool ks_connected() { return g_connected; }
const char* ks_klippy_state() { return g_klippy; }
void ks_klippy_message(char* out, size_t n) {
    xSemaphoreTake(g_lock, portMAX_DELAY); strlcpy(out, g_klippy_msg, n); xSemaphoreGive(g_lock);
}

template <typename T, typename F> static T with_lock(T def, F f) {
    xSemaphoreTake(g_lock, portMAX_DELAY);
    T v = f(def);
    xSemaphoreGive(g_lock);
    return v;
}
bool ks_has(const char* obj) { return with_lock(false, [&](bool) { return !g_status[obj].isNull(); }); }
float ks_f(const char* obj, const char* field, float def) {
    return with_lock(def, [&](float d) { JsonVariantConst v = g_status[obj][field]; return v.is<float>() ? v.as<float>() : d; });
}
float ks_fi(const char* obj, const char* field, int idx, float def) {
    return with_lock(def, [&](float d) { JsonVariantConst v = g_status[obj][field][idx]; return v.is<float>() ? v.as<float>() : d; });
}
bool ks_b(const char* obj, const char* field, bool def) {
    return with_lock(def, [&](bool d) { JsonVariantConst v = g_status[obj][field]; return v.is<bool>() ? v.as<bool>() : d; });
}
bool ks_str(const char* obj, const char* field, char* out, size_t n) {
    return with_lock(false, [&](bool) {
        JsonVariantConst v = g_status[obj][field];
        if (!v.is<const char*>()) return false;
        strlcpy(out, v.as<const char*>(), n);
        return true;
    });
}
int ks_objects(const char* prefix, char names[][40], int max) {
    return with_lock(0, [&](int) {
        int c = 0; size_t pl = strlen(prefix);
        for (JsonVariantConst o : g_status["__objects"].as<JsonArrayConst>()) {
            const char* n = o.as<const char*>();
            if (n && !strncmp(n, prefix, pl) && c < max) strlcpy(names[c++], n, 40);
        }
        return c;
    });
}
int ks_macros(char names[][40], int max) {
    return with_lock(0, [&](int) {
        int c = 0;
        for (JsonVariantConst o : g_status["__macros"].as<JsonArrayConst>()) {
            const char* n = o.as<const char*>();
            if (n && n[0] != '_' && c < max) strlcpy(names[c++], n, 40);
        }
        return c;
    });
}
void ks_with_status(void (*fn)(JsonObjectConst, void*), void* user) {
    xSemaphoreTake(g_lock, portMAX_DELAY);
    fn(g_status.as<JsonObjectConst>(), user);
    xSemaphoreGive(g_lock);
}

// ---- commands --------------------------------------------------------------------------------
static void enqueue(const char* path, const char* body, bool post, ks_get_cb cb, void* user, ks_bytes_cb bcb = nullptr) {
    Req r{strdup(path), body ? strdup(body) : nullptr, post, cb, user, bcb};
    if (xQueueSend(g_req_q, &r, pdMS_TO_TICKS(100)) != pdTRUE) { free(r.path); free(r.body); }
}
void ks_gcode(const char* fmt, ...) {
    char buf[256];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof buf, fmt, ap); va_end(ap);
    JsonDocument d; d["script"] = buf;
    String body; serializeJson(d, body);
    enqueue("/printer/gcode/script", body.c_str(), true, nullptr, nullptr);
}
void ks_post(const char* path, const char* json_body) { enqueue(path, json_body, true, nullptr, nullptr); }
void ks_get_async(const char* path, ks_get_cb cb, void* user) { enqueue(path, nullptr, false, cb, user); }
void ks_post_async(const char* path, const char* json_body, ks_get_cb cb, void* user) { enqueue(path, json_body, true, cb, user); }

void ks_get_bytes_async(const char* path, ks_bytes_cb cb, void* user) { enqueue(path, nullptr, false, nullptr, user, cb); }
void ks_free_bytes(void* data) { if (data) heap_caps_free(data); }

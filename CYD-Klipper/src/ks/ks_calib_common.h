// Private helpers shared by the "calibration and devices" panels (bed_level, zcalibrate, pins, led).
// The Moonraker configfile document is ~50 KB of JSON: far too big for the ~100 KB internal heap as a normal
// ks_get_async() document. It is downloaded into PSRAM (ks_get_bytes_async) and parsed with an ArduinoJson filter
// that keeps only the requested sections / keys.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "ks_mr.h"

// cfg = configfile.config (section names are lower-case, as Moonraker reports them); null object on failure.
typedef void (*kc_cb)(JsonObjectConst cfg, bool ok, void* user);

struct KcCtx {
    const uint32_t* gen_ptr; uint32_t gen;      // the panel bumps *gen_ptr when it is created and deleted
    kc_cb cb; void* user;
    JsonDocument filter;
};

static inline void kc_bytes(bool ok, uint8_t* data, size_t len, void* u) {
    KcCtx* c = (KcCtx*)u;
    if (*c->gen_ptr == c->gen) {                // panel still alive
        JsonDocument doc;
        bool parsed = ok && data &&
            !deserializeJson(doc, (const char*)data, len, DeserializationOption::Filter(c->filter));
        JsonObjectConst cfg = parsed ? doc["result"]["status"]["configfile"]["config"].as<JsonObjectConst>() : JsonObjectConst();
        c->cb(cfg, parsed, c->user);
    }
    ks_free_bytes(data);
    delete c;
}

// sections: whole sections to keep (lower-case); keys: keys to keep in EVERY section.
static inline void kc_fetch(const uint32_t* gen_ptr, const char* const* sections, int ns,
                            const char* const* keys, int nk, kc_cb cb, void* user) {
    KcCtx* c = new KcCtx();
    c->gen_ptr = gen_ptr; c->gen = *gen_ptr; c->cb = cb; c->user = user;
    JsonObject cfg = c->filter["result"]["status"]["configfile"]["config"].to<JsonObject>();
    for (int i = 0; i < ns; i++) cfg[sections[i]] = true;
    for (int i = 0; i < nk; i++) cfg["*"][keys[i]] = true;
    ks_get_bytes_async("/printer/objects/query?configfile=config", kc_bytes, c);
}

static inline const char* kc_val(JsonObjectConst cfg, const char* section, const char* key) {
    return cfg[section][key].as<const char*>();
}
static inline float kc_float(JsonObjectConst cfg, const char* section, const char* key, float def) {
    const char* s = kc_val(cfg, section, key);
    return s ? (float)atof(s) : def;
}

// ---- gcode responses --------------------------------------------------------------------------------------------------
// KlipperScreen gets notify_gcode_response over the websocket; we only have HTTP, so poll /server/gcode_store.
// cb is called (LVGL thread) once per NEW response line, oldest first. The first poll only records the newest timestamp.
typedef void (*kc_msg_cb)(const char* msg, void* user);
struct KcStore { lv_timer_t* t; double last; bool first; kc_msg_cb cb; void* user; const uint32_t* gen_ptr; };
struct KcStoreReq { KcStore* s; const uint32_t* gen_ptr; uint32_t gen; };

static inline void kc_store_reply(bool ok, JsonDocument& doc, void* u) {
    KcStoreReq* r = (KcStoreReq*)u;
    if (ok && *r->gen_ptr == r->gen) {
        KcStore* s = r->s;
        double newest = s->last;
        for (JsonObjectConst m : doc["result"]["gcode_store"].as<JsonArrayConst>()) {
            double t = m["time"] | 0.0;
            if (t <= s->last) continue;
            if (t > newest) newest = t;
            if (!s->first && !strcmp(m["type"] | "", "response")) s->cb(m["message"] | "", s->user);
        }
        s->last = newest;
        s->first = false;
    }
    delete r;
}
static inline void kc_store_tick(lv_timer_t* t) {
    KcStore* s = (KcStore*)t->user_data;
    ks_get_async("/server/gcode_store?count=25", kc_store_reply, new KcStoreReq{s, s->gen_ptr, *s->gen_ptr});
}
static inline KcStore* kc_store_start(const uint32_t* gen_ptr, kc_msg_cb cb, void* user, uint32_t period_ms) {
    KcStore* s = new KcStore{nullptr, 0, true, cb, user, gen_ptr};
    s->t = lv_timer_create(kc_store_tick, period_ms, s);
    kc_store_tick(s->t);
    return s;
}
// call from the panel's DELETE handler AFTER bumping *gen_ptr
static inline void kc_store_stop(KcStore* s) { if (!s) return; lv_timer_del(s->t); delete s; }

// Moonraker client for the KlipperScreen port: polls printer objects into one merged status
// document (what KlipperScreen calls printer.data) and runs commands/requests off the LVGL thread.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include "lvgl.h"

void ks_mr_start();              // spawn the worker task (idempotent)
void ks_mr_tick_init();          // LVGL-side timer: delivers async results + state notifications

void ks_poll_enable(bool on);     // diagnostics: pause/resume the 1 Hz status poll
void ks_retry();                 // drop the connection and re-handshake now
bool ks_connected();             // Moonraker reachable and Klippy answering
const char* ks_klippy_state();   // "ready" | "startup" | "shutdown" | "error" | "disconnected"
void ks_klippy_message(char* out, size_t n);

// ---- status access (thread-safe, copies out) --------------------------------------------
bool  ks_has(const char* obj);                                   // is this printer object present
float ks_f(const char* obj, const char* field, float def = 0);   // number
float ks_fi(const char* obj, const char* field, int idx, float def = 0);  // array element
bool  ks_b(const char* obj, const char* field, bool def = false);
bool  ks_str(const char* obj, const char* field, char* out, size_t n);    // false if missing
int   ks_objects(const char* prefix, char names[][40], int max);          // object names starting with prefix
int   ks_macros(char names[][40], int max);                               // gcode_macro names (no leading '_')
// run fn(root) with the status lock held, for panels that need to walk nested data (bed_mesh, config)
void  ks_with_status(void (*fn)(JsonObjectConst root, void* user), void* user);

// ---- commands ---------------------------------------------------------------------------
void ks_gcode(const char* fmt, ...) __attribute__((format(printf, 1, 2)));  // POST /printer/gcode/script
void ks_post(const char* path, const char* json_body);                      // fire-and-forget POST

typedef void (*ks_get_cb)(bool ok, JsonDocument& doc, void* user);          // runs in the LVGL thread
void ks_get_async(const char* path, ks_get_cb cb, void* user);
void ks_post_async(const char* path, const char* json_body, ks_get_cb cb, void* user);

// Binary download (thumbnails, snapshots) into PSRAM, max 3 MB. The callback (LVGL thread) takes ownership of `data`
// and must release it with ks_free_bytes(); data == nullptr when ok is false.
typedef void (*ks_bytes_cb)(bool ok, uint8_t* data, size_t len, void* user);
void ks_get_bytes_async(const char* path, ks_bytes_cb cb, void* user);
void ks_free_bytes(void* data);

// ---- state-change notification (LVGL thread) ---------------------------------------------
#define KS_MSG_STATE 0x4B53   // lv_msg id sent ~1/s while connected and on connect/disconnect

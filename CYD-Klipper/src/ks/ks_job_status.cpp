// panels/job_status.py (Sovol patch/klipperscreen/job_status.py) + fine_tune.py + plr.py (power-loss recovery dialog).
// Layout: progress ring + name/status, thumbnail | 4x2 info buttons (temps, speed, z, flow, fan, elapsed, left), state buttons.
// Sub pages: Fine Tuning (z babystep / speed / flow), detail lists behind the info buttons.
//
// Exposed for the shell (declare with extern):
//   bool ks_job_active();       printing or paused (print_stats.state). Also arms the background state watcher.
//   bool ks_job_end_pending();  a print just ended (complete / cancelled / error) and the user has not dismissed the end screen yet
//   void ks_job_dismiss();      clear that flag (the "Main Menu" button and the timeouts do it)
//   void ks_plr_check();        power-loss-recovery dialog, only on hosts with the Sovol macros (see below)
//
// Sovol-host dependencies (all gated: skipped when the macro is absent, e.g. on the stock SV08 bench printer):
//   macros PRINT_START / PRINT_END / save_last_file (called once per state transition, the Python calls them on every update),
//   macros resume_interrupted / clear_last_file, save_variables.was_interrupted, RUN_SHELL_COMMAND CMD=clear_plr (shell script).
#include <Arduino.h>
#include <math.h>
#include <string>
#include <strings.h>
#include <time.h>
#include "ks_widgets.h"
#include "ks_mr.h"
#include "ks_cfg.h"
#include "ks_panels.h"

void ks_panel_exclude(lv_obj_t* c);                 // ks_exclude.cpp
bool ks_job_active();
bool ks_job_end_pending();
void ks_job_dismiss();
void ks_plr_check();

#define THUMB_MAX_PIX 2304      // decoded pixels the 40 KB LVGL heap can afford (see ks_print.cpp)

namespace {

enum { ST_STANDBY, ST_PRINTING, ST_PAUSED, ST_COMPLETE, ST_CANCELLED, ST_ERROR, ST_CANCELLING };

// ---- small helpers --------------------------------------------------------------------------------------------------------
std::string urlenc(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') o += (char)c;
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}
void fmt_time(float s, char* o, size_t n) {         // ScreenPanel.format_time
    if (s < 1) { snprintf(o, n, "-"); return; }
    long t = (long)s;
    int d = t / 86400; t %= 86400;
    int h = t / 3600; t %= 3600;
    int m = (int)lroundf(t / 60.0f);
    int sec = t % 60;
    char b[40] = ""; size_t k = 0;
    if (d > 0) k += snprintf(b + k, sizeof b - k, "%dd ", d);
    if (h > 0) k += snprintf(b + k, sizeof b - k, "%dh ", h);
    if (m > 0) k += snprintf(b + k, sizeof b - k, "%dm ", m);
    if (d == 0 && h == 0 && m == 0) snprintf(b + k, sizeof b - k, "%ds", sec);
    size_t l = strlen(b); if (l && b[l - 1] == ' ') b[l - 1] = 0;
    strlcpy(o, b, n);
}
void fmt_eta(float total, float elapsed, char* o, size_t n) {   // "1h 5m | 14:35  +1d"
    float rem = total - elapsed;
    if (rem <= 0) { snprintf(o, n, "-"); return; }
    char left[40]; fmt_time(rem, left, sizeof left);
    time_t t = time(nullptr) + (time_t)rem;
    struct tm tm; localtime_r(&t, &tm);
    char clk[16];
    strftime(clk, sizeof clk, ks_cfg_bool("24htime", true) ? "%H:%M" : "%I:%M %p", &tm);
    int days = (int)(rem / 86400);
    if (days > 0) snprintf(o, n, "%s | %s +%dd", left, clk, days); else snprintf(o, n, "%s | %s", left, clk);
}
bool macro_exists(const char* name) {
    static char names[80][40];
    int n = ks_macros(names, 80);
    for (int i = 0; i < n; i++) if (!strcasecmp(names[i], name)) return true;
    return false;
}
bool raw_active() {
    char st[16];
    return ks_str("print_stats", "state", st, sizeof st) && (!strcmp(st, "printing") || !strcmp(st, "paused"));
}

// ---- generic dialog (ks_confirm only has two fixed buttons) --------------------------------------------------------------
struct DBtn { const char* icon; const char* label; int color; void (*fn)(); };
lv_obj_t* g_dov = nullptr;

lv_obj_t* overlay() {
    if (g_dov) lv_obj_del(g_dov);
    lv_obj_t* ov = lv_obj_create(lv_layer_top());
    g_dov = ov;
    lv_obj_set_size(ov, 800, 480);
    lv_obj_set_style_bg_color(ov, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ov, LV_OPA_60, 0);
    lv_obj_set_style_border_width(ov, 0, 0);
    lv_obj_set_style_radius(ov, 0, 0);
    lv_obj_clear_flag(ov, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(ov, [](lv_event_t*) { g_dov = nullptr; }, LV_EVENT_DELETE, nullptr);
    return ov;
}
lv_obj_t* card(lv_obj_t* ov, int w, int h) {
    lv_obj_t* c = lv_obj_create(ov);
    lv_obj_set_size(c, w, h);
    lv_obj_center(c);
    lv_obj_set_style_bg_color(c, lv_color_black(), 0);
    lv_obj_set_style_border_width(c, 2, 0);
    lv_obj_set_style_border_color(c, KS_ACTIVE, 0);
    lv_obj_set_style_radius(c, 0, 0);
    lv_obj_set_style_pad_all(c, 20, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, 14, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}
void dbtn_click(lv_event_t* e) {
    const DBtn* b = (const DBtn*)lv_event_get_user_data(e);
    void (*fn)() = b->fn;
    if (g_dov) lv_obj_del(g_dov);
    if (fn) fn();
}
void dialog(const char* title, const char* text, const DBtn* b, int n) {
    lv_obj_t* c = card(overlay(), 640, LV_SIZE_CONTENT);
    lv_obj_t* t = ks_label(c, title, &ks_font_28);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP); lv_obj_set_width(t, lv_pct(100));
    if (text && *text) {
        lv_obj_t* x = ks_label(c, text, &ks_font_22);
        lv_label_set_long_mode(x, LV_LABEL_LONG_WRAP); lv_obj_set_width(x, lv_pct(100));
        lv_obj_set_style_text_align(x, LV_TEXT_ALIGN_CENTER, 0);
    }
    lv_obj_t* row = lv_obj_create(c);
    lv_obj_set_size(row, lv_pct(100), 90);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < n; i++) {
        lv_obj_t* k = ks_button_sized(row, b[i].icon, 40, b[i].label, b[i].color, dbtn_click, (void*)&b[i]);
        lv_obj_set_size(k, 580 / n - 8, 84);
    }
}

// ---- state shared by the panel and the background watcher --------------------------------------------------------------
struct Meta {
    bool have = false;
    float est = -1, fil_total = -1, oheight = 0, layer_h = 0, first_h = 0;
    int layer_count = 0, tw = 0, th = 0;
    std::string thumb;
};
Meta g_meta;
std::string g_file;                      // print_stats.filename
uint32_t g_mgen = 0;                     // invalidates metadata / thumbnail answers when the file changes
uint32_t g_meta_retry_at = 0;
int g_meta_tries = 0;
uint8_t* g_png = nullptr; size_t g_png_len = 0;
lv_img_dsc_t g_dsc;

int g_eff = -1;                          // effective state (print_stats.state + local "cancelling" / "cancelled")
bool g_cancelling = false, g_end_pending = false;
lv_timer_t* g_to = nullptr;
bool g_armed = false, g_plr_asked = false;

struct Live {
    float ext_t = 0, ext_tg = 0, bed_t = 0, bed_tg = 0;
    int speed = 100, extr = 100;
    float req = 0, vel = 0, accel = 0, zoff = 0, pos_z = 0, lp[3] = {0, 0, 0}, pa = 0, flow = 0, fil_used = 0;
    float total = 0, pd = 0, prog = 0, est = 0, slicer = -1, filament = -1, file = -1;
    int layer = 0, layers = 0, nobj = 0;
    bool have_layer = false;
    char fans[24] = "";
    char left[64] = "-";
};
Live g_l;
constexpr float FILA_SECTION = 2.405f;   // pi * (1.75/2)^2; configfile is not polled, so the filament diameter cannot be read

void plr_reset() {                       // modify_cfg_value('was_interrupted', 'False') in the Python
    if (macro_exists("resume_interrupted")) ks_gcode("SAVE_VARIABLE VARIABLE=was_interrupted VALUE=False");
}

// ---- metadata + thumbnail of the running file ----------------------------------------------------------------------------
void meta_load();
void png_cb(bool ok, uint8_t* data, size_t len, void* u) {
    if ((uint32_t)(uintptr_t)u != g_mgen || !ok || !data) { ks_free_bytes(data); return; }
    if (g_png) ks_free_bytes(g_png);
    g_png = data; g_png_len = len;
}
void meta_cb(bool ok, JsonDocument& d, void* u) {
    if ((uint32_t)(uintptr_t)u != g_mgen) return;
    if (!ok) { g_meta_retry_at = millis() + 5000; return; }             // new upload: metadata is generated a moment later
    JsonObjectConst r = d["result"];
    Meta& m = g_meta;
    m.have = true;
    m.est = r["estimated_time"] | -1.0f;
    m.fil_total = r["filament_total"] | -1.0f;
    m.oheight = r["object_height"] | 0.0f;
    m.layer_h = r["layer_height"] | 0.0f;
    m.first_h = r["first_layer_height"] | m.layer_h;
    m.layer_count = r["layer_count"] | 0;
    long best = -1;
    for (JsonObjectConst t : r["thumbnails"].as<JsonArrayConst>()) {
        int w = t["width"] | 0, h = t["height"] | 0; const char* p = t["relative_path"] | "";
        long a = (long)w * h;
        if (w && h && *p && a <= THUMB_MAX_PIX && a > best) { best = a; m.thumb = p; m.tw = w; m.th = h; }
    }
    if (!m.thumb.empty()) {
        size_t k = g_file.rfind('/');
        std::string dir = k == std::string::npos ? "" : g_file.substr(0, k + 1);
        std::string u2 = "/server/files/gcodes/" + urlenc(dir + m.thumb);
        ks_get_bytes_async(u2.c_str(), png_cb, (void*)(uintptr_t)g_mgen);
    }
}
void meta_load() {
    g_meta = Meta();
    if (g_png) { ks_free_bytes(g_png); g_png = nullptr; }              // an image widget may still point at it: the panel re-checks g_mgen
    g_mgen++;
    g_meta_retry_at = millis() + 5000;
    std::string u = "/server/files/metadata?filename=" + urlenc(g_file);
    ks_get_async(u.c_str(), meta_cb, (void*)(uintptr_t)g_mgen);
}

// ---- state machine (set_state / _add_timeout) ----------------------------------------------------------------------------
void to_cb(lv_timer_t*) {
    g_to = nullptr;
    if (!raw_active()) { ks_job_dismiss(); ks_home(); }                 // close_panel(): back to the main menu
}
void arm_timeout(const char* key) {
    int secs = ks_cfg_int(key, 0);
    if (g_to) { lv_timer_del(g_to); g_to = nullptr; }
    if (secs > 0) { g_to = lv_timer_create(to_cb, secs * 1000, nullptr); lv_timer_set_repeat_count(g_to, 1); }
}
const DBtn BTN_OK[] = {{"complete", "OK", 3, nullptr}};

int eff_from_raw(const char* st) {
    if (!strcmp(st, "printing")) return g_cancelling ? ST_CANCELLING : ST_PRINTING;
    if (!strcmp(st, "paused")) return g_cancelling ? ST_CANCELLING : ST_PAUSED;
    if (!strcmp(st, "complete")) return ST_COMPLETE;
    if (!strcmp(st, "error")) return ST_ERROR;
    if (!strcmp(st, "cancelled")) return ST_CANCELLED;
    if (g_eff == ST_CANCELLING || g_eff == ST_CANCELLED) return ST_CANCELLED;   // standby right after a cancel
    return ST_STANDBY;
}
void track() {                           // runs from the panel and from the background watcher; idempotent
    char st[16];
    if (!ks_str("print_stats", "state", st, sizeof st)) return;
    int ne = eff_from_raw(st);
    if (strcmp(st, "printing") && strcmp(st, "paused")) g_cancelling = false;
    int prev = g_eff;
    if (ne != prev) {
        g_eff = ne;
        if (prev >= 0) {                                                // do not replay side effects for whatever state was current at boot
            if (ne == ST_PRINTING && prev != ST_PAUSED) {
                g_end_pending = false;
                if (macro_exists("PRINT_START")) ks_gcode("PRINT_START");
                if (macro_exists("save_last_file")) ks_gcode("save_last_file");
            } else if (ne == ST_COMPLETE) {
                g_end_pending = true;
                g_l.prog = 1;
                if (macro_exists("PRINT_END")) ks_gcode("PRINT_END");
                plr_reset();
                arm_timeout("job_complete_timeout");
                ks_toast("Print complete");
            } else if (ne == ST_CANCELLED) {
                g_end_pending = true;
                if (macro_exists("PRINT_END")) ks_gcode("PRINT_END");
                plr_reset();
                arm_timeout("job_cancelled_timeout");
                ks_toast("Print cancelled");
            } else if (ne == ST_ERROR) {
                g_end_pending = true;
                char msg[200] = "";
                ks_str("print_stats", "message", msg, sizeof msg);
                dialog("Print error", msg, BTN_OK, 1);
                arm_timeout("job_error_timeout");
            }
        }
    }
    char fn[200];
    if (ks_str("print_stats", "filename", fn, sizeof fn) && fn[0] && g_file != fn) { g_file = fn; g_meta_tries = 0; meta_load(); }
    else if (!g_file.empty() && !g_meta.have && g_meta_tries < 6 && (int32_t)(millis() - g_meta_retry_at) > 0) { g_meta_tries++; meta_load(); }
}

// ---- power-loss recovery (plr.py) ------------------------------------------------------------------------------------------
void plr_ok() { ks_gcode("resume_interrupted"); }
void plr_cancel() {
    if (macro_exists("clear_last_file")) ks_gcode("clear_last_file");
    ks_gcode("SAVE_VARIABLE VARIABLE=was_interrupted VALUE=False");
    ks_gcode("RUN_SHELL_COMMAND CMD=clear_plr");                        // gcode_shell_command + script: only exists on the Sovol host
    ks_home();
}
const DBtn BTN_PLR[] = {{"cancel", "Cancel", 4, plr_cancel}, {"complete", "OK", 3, plr_ok}};

bool was_interrupted() {
    bool v = false;
    ks_with_status([](JsonObjectConst root, void* u) {
        JsonVariantConst x = root["save_variables"]["variables"]["was_interrupted"];
        bool* r = (bool*)u;
        if (x.is<bool>()) *r = x.as<bool>();
        else if (x.is<const char*>()) *r = !strcasecmp(x.as<const char*>(), "true");
    }, &v);
    return v;
}
void on_msg(void*, lv_msg_t*) {          // global KS_MSG_STATE subscriber: keeps the state machine running when job_status is not on screen
    if (!ks_connected()) { g_plr_asked = false; return; }
    track();
    if (!g_plr_asked && !raw_active() && macro_exists("resume_interrupted")) {
        if (was_interrupted()) { g_plr_asked = true; ks_plr_check(); }
    }
}
void arm() {
    if (g_armed) return;
    g_armed = true;
    lv_msg_subscribe(KS_MSG_STATE, on_msg, nullptr);
}

// ---- live values ------------------------------------------------------------------------------------------------------------
void snap_info(JsonObjectConst root, void*) {
    JsonObjectConst inf = root["print_stats"]["info"];
    g_l.have_layer = false;
    g_l.layers = inf["total_layer"].is<int>() ? inf["total_layer"].as<int>() : 0;
    if (inf["current_layer"].is<int>()) { g_l.layer = inf["current_layer"].as<int>(); g_l.have_layer = true; }
    g_l.nobj = root["exclude_object"]["objects"].size();
}
void compute() {
    Live& l = g_l;
    char ext[40] = "extruder"; ks_str("toolhead", "extruder", ext, sizeof ext);
    l.ext_t = ks_f(ext, "temperature"); l.ext_tg = ks_f(ext, "target"); l.pa = ks_f(ext, "pressure_advance");
    l.bed_t = ks_f("heater_bed", "temperature"); l.bed_tg = ks_f("heater_bed", "target");
    float sf = ks_f("gcode_move", "speed_factor", 1);
    l.speed = (int)lroundf(sf * 100);
    l.extr = (int)lroundf(ks_f("gcode_move", "extrude_factor", 1) * 100);
    l.req = roundf(ks_f("gcode_move", "speed") / 60 * sf);
    l.vel = ks_f("motion_report", "live_velocity");
    l.accel = ks_f("toolhead", "max_accel");
    l.zoff = ks_fi("gcode_move", "homing_origin", 2);
    l.pos_z = roundf(ks_fi("gcode_move", "gcode_position", 2) * 100) / 100;
    for (int i = 0; i < 3; i++) l.lp[i] = ks_fi("motion_report", "live_position", i);
    l.flow = 0.5f * l.flow + 0.5f * FILA_SECTION * ks_f("motion_report", "live_extruder_velocity");   // 1 Hz samples: light smoothing instead of the 2 s median
    l.fil_used = ks_f("print_stats", "filament_used") / 1000.0f;

    char fb[24] = "";                                                    // fan label: " 68% E:100%" (first letter of each fan_generic)
    float fs = ks_f("fan", "speed", -1);
    if (fs >= 0) snprintf(fb, sizeof fb, "  %3.0f%%", fs * 100);
    char names[6][40];
    int nf = ks_objects("fan_generic ", names, 6);
    for (int i = 0; i < nf; i++) {
        const char* n = names[i] + 12;
        if (*n == '_') continue;
        size_t k = strlen(fb);
        if (k < sizeof fb - 8) snprintf(fb + k, sizeof fb - k, " %c:%3.0f%%", toupper((unsigned char)*n), ks_f(names[i], "speed") * 100);
    }
    fb[12] = 0;
    strlcpy(l.fans, fb, sizeof l.fans);

    ks_with_status(snap_info, nullptr);
    const Meta& m = g_meta;
    if (!l.layers && m.have) {
        if (m.layer_count > 0) l.layers = m.layer_count;
        else if (m.oheight > 0 && m.layer_h > 0) l.layers = (int)lroundf((m.oheight - m.first_h) / m.layer_h + 1);
    }
    if (!l.have_layer && m.have && m.layer_h > 0) { l.layer = 1 + (int)lroundf((l.pos_z - m.first_h) / m.layer_h); if (l.layer < 0) l.layer = 0; }

    // update_time_left() - only while a job is running
    if (g_eff != ST_PRINTING && g_eff != ST_PAUSED) return;
    l.total = ks_f("print_stats", "total_duration");
    l.pd = ks_f("print_stats", "print_duration");
    float fu = ks_f("print_stats", "filament_used");
    float prog = ks_f("virtual_sdcard", "progress");
    if (m.have && m.fil_total == 0) l.pd = l.total;                      // no-extrusion file
    l.slicer = (m.have && m.est > 0) ? m.est / sqrtf(sf > 0.01f ? sf : 1.0f) : -1;   // sqrt(speed_factor): empirical, from KlipperScreen
    l.filament = (m.have && m.fil_total > fu && fu > 0) ? l.pd / (fu / m.fil_total) : -1;
    l.file = prog > 0 ? l.pd / prog : -1;
    int method = ks_cfg_int("print_estimate_method", 0);                 // 0 auto, 1 file, 2 filament, 3 slicer
    float est = 0;
    if (method == 1) est = l.file;
    else if (method == 2) est = l.filament;
    else if (l.slicer >= 0) {
        if (method == 3) est = l.slicer;
        else if (l.filament >= 0 && prog > 0.14f) est = (l.slicer * 3 + l.filament + l.file) / 5;   // weighted mean, slicer is the most accurate
        else est = l.slicer;
    }
    if (est < 1) {
        if (l.file < 0) return;
        est = l.file;
    }
    if (est < 1) return;
    float p = l.pd / est; p = p < 0 ? 0 : p > 1 ? 1 : p;
    if (p >= 1 && l.file > 0) { est = l.file; p = l.pd / est; p = p < 0 ? 0 : p > 1 ? 1 : p; }
    l.est = est; l.prog = p;
    fmt_eta(est, l.pd, l.left, sizeof l.left);
}

// ---- Fine Tuning (panels/fine_tune.py) --------------------------------------------------------------------------------------
const char* const Z_D[] = {"0.01", "0.05"};
const char* const S_D[] = {"5%", "25%"};
const char* const E_D[] = {"1%", "2%"};
int g_zd = 1, g_sd = 1, g_ed = 1;
int ft_speed = 100, ft_ext = 100; float ft_z = 0;
uint32_t ft_hold = 0;                    // ignore status echoes for a moment after a press (the button shows the optimistic value)
lv_obj_t *ft_zl = nullptr, *ft_sl = nullptr, *ft_el = nullptr;

void ft_text() {
    if (!ft_zl) return;
    char b[24];
    snprintf(b, sizeof b, "%.3f mm", ft_z); lv_label_set_text(ft_zl, b);
    snprintf(b, sizeof b, "%d%%", ft_speed); lv_label_set_text(ft_sl, b);
    snprintf(b, sizeof b, "%d%%", ft_ext); lv_label_set_text(ft_el, b);
}
void ft_hold_set() { ft_hold = millis() + 2500; }
void ft_z_cb(lv_event_t* e) {
    int dir = (int)(intptr_t)lv_event_get_user_data(e);
    if (!dir) { ft_z = 0; ks_gcode("SET_GCODE_OFFSET Z=0 MOVE=1"); }
    else { ft_z += dir * atof(Z_D[g_zd]); ks_gcode("SET_GCODE_OFFSET Z_ADJUST=%c%s MOVE=1", dir > 0 ? '+' : '-', Z_D[g_zd]); }
    ft_hold_set(); ft_text();
}
void ft_speed_cb(lv_event_t* e) {
    int dir = (int)(intptr_t)lv_event_get_user_data(e);
    ft_speed = dir ? ft_speed + dir * atoi(S_D[g_sd]) : 100;
    if (ft_speed < 1) ft_speed = 1;
    ks_gcode("M220 S%d", ft_speed);
    ft_hold_set(); ft_text();
}
void ft_ext_cb(lv_event_t* e) {
    int dir = (int)(intptr_t)lv_event_get_user_data(e);
    ft_ext = dir ? ft_ext + dir * atoi(E_D[g_ed]) : 100;
    if (ft_ext < 1) ft_ext = 1;
    ks_gcode("M221 S%d", ft_ext);
    ft_hold_set(); ft_text();
}
void ft_state(lv_event_t*) {
    if ((int32_t)(millis() - ft_hold) < 0) return;
    ft_z = ks_fi("gcode_move", "homing_origin", 2);
    ft_speed = (int)lroundf(ks_f("gcode_move", "speed_factor", 1) * 100);
    ft_ext = (int)lroundf(ks_f("gcode_move", "extrude_factor", 1) * 100);
    ft_text();
}
lv_obj_t* ft_btn(lv_obj_t* g, const char* icon, const char* label, int color, lv_event_cb_t cb, int dir, int col, int row) {
    lv_obj_t* b = ks_button_sized(g, icon, 40, label, color, cb, (void*)(intptr_t)dir);
    lv_obj_set_grid_cell(b, LV_GRID_ALIGN_STRETCH, col, 1, LV_GRID_ALIGN_STRETCH, row, 1);
    return b;
}
void fine_tune_init(lv_obj_t* c) {
    static lv_coord_t cw[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
    static lv_coord_t rh[] = {LV_GRID_FR(3), LV_GRID_FR(3), LV_GRID_FR(3), LV_GRID_FR(2), LV_GRID_TEMPLATE_LAST};
    lv_obj_set_grid_dsc_array(c, cw, rh);
    lv_obj_set_style_pad_all(c, 2, 0);
    lv_obj_set_style_pad_gap(c, 2, 0);
    // column 0: z offset, 1: speed, 2: extrusion  (rows: value / + / - / step toggles)
    lv_obj_t* z = ft_btn(c, "refresh", "", 1, ft_z_cb, 0, 0, 0);
    ft_btn(c, "z-farther", "Z+", 1, ft_z_cb, 1, 0, 1);
    ft_btn(c, "z-closer", "Z-", 1, ft_z_cb, -1, 0, 2);
    lv_obj_t* s = ft_btn(c, "refresh", "", 3, ft_speed_cb, 0, 1, 0);
    ft_btn(c, "speed+", "Speed +", 3, ft_speed_cb, 1, 1, 1);
    ft_btn(c, "speed-", "Speed -", 3, ft_speed_cb, -1, 1, 2);
    lv_obj_t* x = ft_btn(c, "refresh", "", 4, ft_ext_cb, 0, 2, 0);
    ft_btn(c, "flow+", "Extrusion +", 4, ft_ext_cb, 1, 2, 1);
    ft_btn(c, "flow-", "Extrusion -", 4, ft_ext_cb, -1, 2, 2);
    ft_zl = lv_obj_get_child(z, 1); ft_sl = lv_obj_get_child(s, 1); ft_el = lv_obj_get_child(x, 1);
    lv_obj_add_event_cb(z, [](lv_event_t*) { ft_zl = ft_sl = ft_el = nullptr; }, LV_EVENT_DELETE, nullptr);
    lv_obj_t* tz = ks_toggles(c, Z_D, 2, g_zd, [](int i, void*) { g_zd = i; }, nullptr);
    lv_obj_t* ts = ks_toggles(c, S_D, 2, g_sd, [](int i, void*) { g_sd = i; }, nullptr);
    lv_obj_t* te = ks_toggles(c, E_D, 2, g_ed, [](int i, void*) { g_ed = i; }, nullptr);
    lv_obj_set_grid_cell(tz, LV_GRID_ALIGN_STRETCH, 0, 1, LV_GRID_ALIGN_STRETCH, 3, 1);
    lv_obj_set_grid_cell(ts, LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_STRETCH, 3, 1);
    lv_obj_set_grid_cell(te, LV_GRID_ALIGN_STRETCH, 2, 1, LV_GRID_ALIGN_STRETCH, 3, 1);
    ft_hold = 0;
    ks_on_state(z, ft_state);
    ft_state(nullptr);
}

// ---- detail lists behind the info buttons (the switch_info() sub grids) ---------------------------------------------------
int g_kind = 0;
lv_obj_t* g_dv[6];
const char* const D_NAMES[3][6] = {
    {"Speed:", "Acceleration:", "Position:", "Z offset:", "Height:", "Layer:"},
    {"Flow:", "Flowrate:", "Pressure Advance:", "Filament used:", "Filament total:", nullptr},
    {"Elapsed:", "Left:", "Total:", "Slicer:", "File:", "Filament:"},
};
void detail_fill() {
    const Live& l = g_l;
    char b[96], t[48];
    for (int i = 0; i < 6 && D_NAMES[g_kind][i]; i++) {
        b[0] = 0;
        if (g_kind == 0) switch (i) {
            case 0: snprintf(b, sizeof b, "%d%% %3.0f/%3.0f mm/s", l.speed, l.vel, l.req); break;
            case 1: snprintf(b, sizeof b, "%.0f mm/s²", l.accel); break;
            case 2: snprintf(b, sizeof b, "X%.1f Y%.1f Z%.2f", l.lp[0], l.lp[1], l.lp[2]); break;
            case 3: snprintf(b, sizeof b, "%.3f mm", l.zoff); break;
            case 4: snprintf(b, sizeof b, g_meta.oheight > 0 ? "%g mm" : "-", g_meta.oheight); break;
            case 5: if (l.layers) snprintf(b, sizeof b, "%d / %d", l.layer, l.layers); else snprintf(b, sizeof b, "-"); break;
        } else if (g_kind == 1) switch (i) {
            case 0: snprintf(b, sizeof b, "%d%%", l.extr); break;
            case 1: snprintf(b, sizeof b, "%.1f mm³/s", l.flow); break;
            case 2: snprintf(b, sizeof b, "%.2f", l.pa); break;
            case 3: snprintf(b, sizeof b, "%.1f m", l.fil_used); break;
            case 4: if (g_meta.fil_total >= 0) snprintf(b, sizeof b, "%.1f m", g_meta.fil_total / 1000); else snprintf(b, sizeof b, "-"); break;
        } else switch (i) {
            case 0: fmt_time(l.total, b, sizeof b); break;
            case 1: snprintf(b, sizeof b, "%s", l.left); break;
            case 2: fmt_time(l.est, b, sizeof b); break;
            case 3: fmt_time(l.slicer, b, sizeof b); break;
            case 4: fmt_time(l.file, b, sizeof b); break;
            case 5: fmt_time(l.filament, b, sizeof b); break;
        }
        (void)t;
        if (g_dv[i]) lv_label_set_text(g_dv[i], b);
    }
}
void detail_state(lv_event_t*) { track(); compute(); detail_fill(); }
void detail_init(lv_obj_t* c) {
    lv_obj_t* list = ks_list(c);
    for (int i = 0; i < 6; i++) g_dv[i] = nullptr;
    for (int i = 0; i < 6 && D_NAMES[g_kind][i]; i++) {
        lv_obj_t* r = ks_row(list, D_NAMES[g_kind][i]);
        lv_obj_t* box = ks_row_right(r, 330);
        g_dv[i] = ks_label(box, "-", &ks_font_22);
    }
    lv_obj_add_event_cb(list, [](lv_event_t*) { for (auto& p : g_dv) p = nullptr; }, LV_EVENT_DELETE, nullptr);
    ks_on_state(list, detail_state);
    detail_fill();
}
void open_detail(lv_event_t* e) {
    g_kind = (int)(intptr_t)lv_event_get_user_data(e);
    static const char* const T[] = {"Motion", "Extrusion", "Time"};
    ks_open(T[g_kind], detail_init);
}

// ---- the panel ------------------------------------------------------------------------------------------------------------------
enum { L_EXT, L_BED, L_SPEED, L_Z, L_FLOW, L_FAN, L_ELAPSED, L_LEFT, L_N };
struct JS {
    lv_obj_t *arc = nullptr, *pct = nullptr, *name = nullptr, *status = nullptr, *lcd = nullptr, *layer = nullptr;
    lv_obj_t *tbox = nullptr, *btnbox = nullptr, *lab[L_N] = {};
    bool has_img = false; uint32_t shown_gen = 0;
    uint32_t sig = 0xFFFFFFFF;
    std::string fname;
};
JS* g_js = nullptr;

void open_temp(lv_event_t*) { ks_open("Temperature", ks_panel_temperature); }
void open_fan(lv_event_t*) { ks_open("Fan", ks_panel_fan); }

void pause_cb(lv_event_t* e) { ks_post("/printer/print/pause", "{}"); lv_obj_add_state(lv_event_get_target(e), LV_STATE_DISABLED); }
void resume_cb(lv_event_t* e) { ks_post("/printer/print/resume", "{}"); lv_obj_add_state(lv_event_get_target(e), LV_STATE_DISABLED); }
void do_exclude() { ks_open("Exclude Object", ks_panel_exclude); }
void do_cancel() {
    g_cancelling = true;
    g_eff = ST_CANCELLING;
    plr_reset();
    ks_post("/printer/print/cancel", "{}");
}
const DBtn BTN_CANCEL3[] = {{"extrude", "Exclude Object", 2, do_exclude}, {"stop", "Cancel Print", 1, do_cancel}, {"back", "Go Back", 3, nullptr}};
const DBtn BTN_CANCEL2[] = {{"stop", "Cancel Print", 1, do_cancel}, {"back", "Go Back", 3, nullptr}};
void cancel_cb(lv_event_t*) {
    if (g_l.nobj > 1) dialog("Cancel", "Are you sure you wish to cancel this print?", BTN_CANCEL3, 3);
    else dialog("Cancel", "Are you sure you wish to cancel this print?", BTN_CANCEL2, 2);
}
void fine_cb(lv_event_t*) { ks_open("Fine Tuning", fine_tune_init); }
void control_cb(lv_event_t*) { ks_menu_open_print(); }
void menu_cb(lv_event_t*) { ks_job_dismiss(); ks_home(); }
void restart_cb(lv_event_t* e) {
    if (g_file.empty()) return;
    lv_obj_add_state(lv_event_get_target(e), LV_STATE_DISABLED);
    if (g_eff == ST_ERROR) ks_gcode("SDCARD_RESET_FILE");
    std::string p = "/printer/print/start?filename=" + urlenc(g_file);
    ks_post(p.c_str(), "{}");
    g_l.prog = 0;
}
void save_ok(void* u) {
    ks_gcode((bool)(intptr_t)u ? "Z_OFFSET_APPLY_PROBE" : "Z_OFFSET_APPLY_ENDSTOP");
    ks_gcode("SAVE_CONFIG");
}
void save_cb(lv_event_t* e) {
    bool probe = (bool)(intptr_t)lv_event_get_user_data(e);
    char t[64];
    snprintf(t, sizeof t, "Apply %c%.3f offset to %s?", g_l.zoff > 0 ? '+' : '-', fabsf(g_l.zoff), probe ? "Probe" : "Endstop");
    ks_confirm("Save Z", t, "Apply", save_ok, (void*)(intptr_t)probe);
}

void show_image(lv_obj_t* box, int target) {
    lv_obj_clean(box);
    lv_obj_t* im = lv_img_create(box);
    lv_img_set_src(im, &g_dsc);
    int m = g_meta.tw > g_meta.th ? g_meta.tw : g_meta.th;
    if (m > 0) lv_img_set_zoom(im, 256 * target / m);
    lv_obj_center(im);
}
void prepare_dsc() {
    memset(&g_dsc, 0, sizeof g_dsc);
    g_dsc.header.cf = LV_IMG_CF_RAW_ALPHA;
    g_dsc.header.w = g_meta.tw; g_dsc.header.h = g_meta.th;
    g_dsc.data_size = g_png_len; g_dsc.data = g_png;
}
void thumb_cb(lv_event_t*) {                         // show_fullscreen_thumbnail()
    if (!g_png) return;
    lv_obj_t* c = card(overlay(), 460, LV_SIZE_CONTENT);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t* t = ks_label(c, g_file.c_str(), &ks_font_22);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT); lv_obj_set_width(t, lv_pct(100));
    lv_obj_t* box = lv_obj_create(c);
    lv_obj_set_size(box, 300, 300);
    lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    prepare_dsc();
    show_image(box, 280);
    static const DBtn close[] = {{"cancel", "Close", 1, nullptr}};
    lv_obj_t* b = ks_button_sized(c, close[0].icon, 40, close[0].label, close[0].color, dbtn_click, (void*)&close[0]);
    lv_obj_set_size(b, 200, 84);
}

lv_obj_t* info_btn(lv_obj_t* grid, const char* icon, lv_event_cb_t cb, intptr_t user) {
    lv_obj_t* b = ks_button(grid, nullptr, nullptr, 0, cb, (void*)user);
    lv_obj_set_size(b, 243, 56);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(b, 6, 0);
    ks_img(b, icon, 40, lv_color_white());
    lv_obj_t* l = ks_label(b, "-", &ks_font_22);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l, 186);
    return l;
}

void set_t(lv_obj_t* l, float t, float tg) {
    char b[24];
    if (tg > 0) snprintf(b, sizeof b, "%.0f/%.0f°", t, tg); else snprintf(b, sizeof b, "%.0f°", t);
    lv_label_set_text(l, b);
}

void show_buttons(JS* s) {                           // show_buttons_for_state(): fixed slots 0..3
    lv_obj_clean(s->btnbox);
    auto add = [&](int slot, const char* icon, int px, const char* label, int color, lv_event_cb_t cb, intptr_t user) {
        lv_obj_t* b = ks_button_sized(s->btnbox, icon, px, label, color, cb, (void*)user);
        lv_obj_set_size(b, 177, 108);
        lv_obj_set_pos(b, slot * 179, 0);
        return b;
    };
    if (g_eff == ST_PRINTING || g_eff == ST_CANCELLING) {
        add(0, "pause", 56, "Pause", 1, pause_cb, 0);
        add(1, "stop", 56, "Cancel", 2, cancel_cb, 0);
        add(2, "fine-tune", 56, "Fine Tuning", 4, fine_cb, 0);
        add(3, "settings", 56, "Settings", 3, control_cb, 0);
        if (g_eff == ST_CANCELLING) { lv_obj_add_state(lv_obj_get_child(s->btnbox, 0), LV_STATE_DISABLED); lv_obj_add_state(lv_obj_get_child(s->btnbox, 1), LV_STATE_DISABLED); }
    } else if (g_eff == ST_PAUSED) {
        add(0, "resume", 56, "Resume", 1, resume_cb, 0);
        add(1, "stop", 56, "Cancel", 2, cancel_cb, 0);
        add(2, "fine-tune", 56, "Fine Tuning", 4, fine_cb, 0);
        add(3, "settings", 56, "Settings", 3, control_cb, 0);
    } else {
        if (fabsf(g_l.zoff) > 0.0005f) {                      // save the babystep as a permanent offset
            if (!ks_has("probe")) add(0, "home-z", 40, "Save Z\nEndstop", 2, save_cb, 0);
            if (ks_has("probe")) add(1, "home-z", 40, "Save Z\nProbe", 1, save_cb, 1);
        }
        if (!g_file.empty()) add(2, "refresh", 56, "Restart", 3, restart_cb, 0);
        add(3, "complete", 56, "Main Menu", 4, menu_cb, 0);
    }
}

void update(JS* s) {
    track();
    compute();
    const Live& l = g_l;
    static const char* const ST[] = {"Standby", "Printing", "Paused", "Complete", "Cancelled", "Error", "Cancelling"};
    lv_label_set_text(s->status, ST[g_eff < 0 ? 0 : g_eff]);

    float p = (g_eff == ST_COMPLETE) ? 1.0f : l.prog;
    if (p == 0 && (g_eff == ST_PRINTING || g_eff == ST_PAUSED) && l.est < 1) p = ks_f("virtual_sdcard", "progress");   // no estimate (yet): file position
    lv_arc_set_value(s->arc, (int)(p * 1000));
    char b[48];
    snprintf(b, sizeof b, "%d%%", (int)(p * 100));
    lv_label_set_text(s->pct, b);

    if (s->fname != g_file) {
        s->fname = g_file;
        std::string n = g_file.substr(0, g_file.rfind('.'));
        size_t k = n.rfind('/'); if (k != std::string::npos) n = n.substr(k + 1);
        lv_label_set_text(s->name, n.empty() ? "Filename" : n.c_str());
    }
    char m[64] = "";
    ks_str("display_status", "message", m, sizeof m);
    lv_label_set_text(s->lcd, m);
    if (l.layers) { snprintf(b, sizeof b, "Layer %d / %d", l.layer, l.layers); lv_label_set_text(s->layer, b); } else lv_label_set_text(s->layer, "");

    set_t(s->lab[L_EXT], l.ext_t, l.ext_tg);
    set_t(s->lab[L_BED], l.bed_t, l.bed_tg);
    snprintf(b, sizeof b, "%d%% %3.0f/%3.0f mm/s", l.speed, l.vel, l.req); lv_label_set_text(s->lab[L_SPEED], b);
    if (g_meta.oheight > 0) snprintf(b, sizeof b, "Z: %6.2f/%g", l.pos_z, g_meta.oheight); else snprintf(b, sizeof b, "Z: %6.2f", l.pos_z);
    lv_label_set_text(s->lab[L_Z], b);
    snprintf(b, sizeof b, "%3d%% %5.1f mm³/s", l.extr, l.flow); lv_label_set_text(s->lab[L_FLOW], b);
    lv_label_set_text(s->lab[L_FAN], l.fans);
    char t[40]; fmt_time(l.total, t, sizeof t);
    snprintf(b, sizeof b, "Elapsed:  %s", t); lv_label_set_text(s->lab[L_ELAPSED], b);
    snprintf(b, sizeof b, "Left:  %s", (g_eff == ST_PRINTING || g_eff == ST_PAUSED) ? l.left : "-"); lv_label_set_text(s->lab[L_LEFT], b);

    if (!g_png && s->has_img) {                          // file changed: g_png was freed, drop the widget that pointed at it
        s->has_img = false;
        lv_obj_clean(s->tbox);
        lv_obj_center(ks_img(s->tbox, "file", 72, lv_color_white()));
    }
    if (g_png && s->shown_gen != g_mgen) {              // the thumbnail arrived (or the file changed)
        s->shown_gen = g_mgen;
        s->has_img = true;
        prepare_dsc();
        show_image(s->tbox, 180);
    }
    uint32_t sig = g_eff | (fabsf(l.zoff) > 0.0005f ? 16 : 0) | (g_file.empty() ? 0 : 32);
    if (sig != s->sig) { s->sig = sig; show_buttons(s); }
}

}   // namespace

bool ks_job_active() { arm(); return raw_active(); }
bool ks_job_end_pending() { return g_end_pending && !raw_active(); }
void ks_job_dismiss() { g_end_pending = false; if (g_to) { lv_timer_del(g_to); g_to = nullptr; } }

void ks_plr_check() {                    // plr.py: shown when the host reports an interrupted print and offers the recovery macro
    arm();
    if (raw_active() || !macro_exists("resume_interrupted") || !was_interrupted()) return;
    g_plr_asked = true;
    dialog("Print recovery", "Abnormal interruption\n of printing is detected,\nwhether to resume", BTN_PLR, 2);
}

void ks_panel_job_status(lv_obj_t* c) {
    arm();
    JS* s = new JS;
    g_js = s;
    lv_obj_t* root = ks_card(c, lv_pct(100), lv_pct(100));
    lv_obj_add_event_cb(root, [](lv_event_t* e) { JS* j = (JS*)lv_event_get_user_data(e); if (g_js == j) g_js = nullptr; delete j; }, LV_EVENT_DELETE, s);

    s->arc = lv_arc_create(root);                     // on_draw(): dark ring + red progress arc starting at 12 o'clock
    lv_obj_set_size(s->arc, 70, 70);
    lv_obj_set_pos(s->arc, 0, 0);
    lv_arc_set_rotation(s->arc, 270);
    lv_arc_set_bg_angles(s->arc, 0, 360);
    lv_arc_set_range(s->arc, 0, 1000);
    lv_obj_remove_style(s->arc, nullptr, LV_PART_KNOB);
    lv_obj_clear_flag(s->arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(s->arc, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s->arc, 0, 0);
    lv_obj_set_style_arc_width(s->arc, 9, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s->arc, 9, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s->arc, lv_color_hex(0x222222), LV_PART_MAIN);
    lv_obj_set_style_arc_color(s->arc, lv_color_hex(0xB71C1C), LV_PART_INDICATOR);
    s->pct = ks_label(s->arc, "0%", &ks_font_18);
    lv_obj_center(s->pct);

    s->name = ks_label(root, "Filename", &ks_font_28);
    lv_label_set_long_mode(s->name, LV_LABEL_LONG_SCROLL_CIRCULAR);   // animate_label()
    lv_obj_set_width(s->name, 630);
    lv_obj_set_pos(s->name, 82, 0);
    s->status = ks_label(root, "Standby", &ks_font_22);
    lv_obj_set_pos(s->status, 82, 38);
    s->lcd = ks_label(root, "", &ks_font_18);
    lv_label_set_long_mode(s->lcd, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s->lcd, 230);
    lv_obj_set_pos(s->lcd, 240, 42);
    s->layer = ks_label(root, "", &ks_font_22);
    lv_obj_set_width(s->layer, 220);
    lv_obj_set_style_text_align(s->layer, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s->layer, 490, 38);

    lv_obj_t* tb = ks_button(root, nullptr, nullptr, 0, thumb_cb, nullptr);     // thumbnail, tap = full view
    lv_obj_set_size(tb, 220, 230);
    lv_obj_set_pos(tb, 0, 72);
    s->tbox = ks_card(tb, 200, 200);
    lv_obj_t* ic = ks_img(s->tbox, "file", 72, lv_color_white());
    lv_obj_center(ic);

    lv_obj_t* grid = ks_card(root, 488, 230);
    lv_obj_set_pos(grid, 228, 72);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(grid, 2, 0);
    s->lab[L_EXT] = info_btn(grid, "extruder", open_temp, 0);
    s->lab[L_BED] = info_btn(grid, "bed", open_temp, 0);
    s->lab[L_SPEED] = info_btn(grid, "speed+", open_detail, 0);
    s->lab[L_Z] = info_btn(grid, "home-z", open_detail, 0);
    s->lab[L_FLOW] = info_btn(grid, "extrude", open_detail, 1);
    s->lab[L_FAN] = info_btn(grid, "fan", open_fan, 0);
    s->lab[L_ELAPSED] = info_btn(grid, "clock", open_detail, 2);
    s->lab[L_LEFT] = info_btn(grid, "hourglass", open_detail, 2);
    char fn[2][40];
    if (!ks_has("fan") && !ks_objects("fan_generic ", fn, 1)) lv_obj_add_flag(lv_obj_get_parent(s->lab[L_FAN]), LV_OBJ_FLAG_HIDDEN);
    if (!ks_has("heater_bed")) lv_obj_add_flag(lv_obj_get_parent(s->lab[L_BED]), LV_OBJ_FLAG_HIDDEN);

    s->btnbox = ks_card(root, 716, 110);
    lv_obj_set_pos(s->btnbox, 0, 306);

    ks_on_state(root, [](lv_event_t*) { if (g_js) update(g_js); });
    update(s);
}

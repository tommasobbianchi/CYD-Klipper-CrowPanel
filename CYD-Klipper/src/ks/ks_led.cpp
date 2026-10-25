// panels/led.py: led / neopixel / dotstar / pca9533 / pca9632 devices (names starting with '_' hidden).
// One device: straight to the colour page; several: a selector of coloured buttons first (the action-bar back returns to it).
// Colour page: one slider (0-255) + full-intensity swatch button per channel present in the LED's colour order, a live preview
// with the hex value, and presets ("off", "on" and the Mainsail presets from the Moonraker database).
// Every change is SET_LED LED="name" RED=.. GREEN=.. BLUE=.. WHITE=.. SYNC=0 TRANSMIT=1 (KlippyGcodes.set_led_color).
#include <Arduino.h>
#include <math.h>
#include "ks_widgets.h"
#include "ks_mr.h"
#include "ks_panels.h"
#include "ks_calib_common.h"

#define MAXLED 16
static const char* const TYPES[] = {"dotstar ", "led ", "neopixel ", "pca9533 ", "pca9632 "};

// colour order per LED, filled when the selector panel loads the config and read again by the colour page
struct Ord { char led[40]; char order[12]; };
static Ord g_ord[MAXLED];
static int g_nord;
static char g_open_led[40];

static const char* led_short(const char* led) { const char* sp = strchr(led, ' '); return sp ? sp + 1 : led; }
static int list_leds(char names[][40]) {
    int n = 0;
    for (const char* t : TYPES) {
        char tmp[MAXLED][40];
        int c = ks_objects(t, tmp, MAXLED);
        for (int i = 0; i < c && n < MAXLED; i++) if (led_short(tmp[i])[0] != '_') strlcpy(names[n++], tmp[i], 40);
    }
    return n;
}

// ---- colour page -----------------------------------------------------------------------------------------------------------------
struct Preset { char name[20]; float c[4]; };
struct CP {
    lv_obj_t *root, *prev, *hex, *plist;
    lv_obj_t* sl[4]; lv_obj_t* sw[4];
    char led[40]; char order[12];
    float col[4];
    Preset pre[16]; int np;
};
static CP* g_cp;
static uint32_t g_gen_cp, g_gen;

static bool avail(const CP* s, int idx) { return strchr(s->order, "RGBW"[idx]) != nullptr; }
static lv_color_t rgbw_to_rgb(const float* c) {            // the white channel washes the colour out
    float r, g, b;
    if (c[0] == 0 && c[1] == 0 && c[2] == 0) r = g = b = c[3];
    else { r = c[0] + (1 - c[0]) * c[3] / 3; g = c[1] + (1 - c[1]) * c[3] / 3; b = c[2] + (1 - c[2]) * c[3] / 3; }
    return lv_color_make((uint8_t)(r * 255 + 0.5f), (uint8_t)(g * 255 + 0.5f), (uint8_t)(b * 255 + 0.5f));
}
static void swatch_style(lv_obj_t* o, lv_color_t c) {
    lv_obj_set_style_bg_color(o, c, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, 12, 0);
    lv_obj_set_style_border_width(o, 2, 0);
    lv_obj_set_style_border_color(o, KS_ACTIVE, 0);
}
static void update_preview(CP* s) {
    for (int i = 0; i < 4; i++) if (s->sl[i]) s->col[i] = lv_slider_get_value(s->sl[i]) / 255.0f;
    lv_obj_set_style_bg_color(s->prev, rgbw_to_rgb(s->col), 0);
    float r8[3];
    if (s->col[0] == 0 && s->col[1] == 0 && s->col[2] == 0) r8[0] = r8[1] = r8[2] = s->col[3];
    else for (int i = 0; i < 3; i++) r8[i] = s->col[i] + (1 - s->col[i]) * s->col[3] / 3;
    char b[12];
    snprintf(b, sizeof b, "#%02X%02X%02X", (int)(r8[0] * 255 + 0.5f), (int)(r8[1] * 255 + 0.5f), (int)(r8[2] * 255 + 0.5f));
    lv_label_set_text(s->hex, b);
}
static void send_color(CP* s) {
    ks_gcode("SET_LED LED=\"%s\" RED=%g GREEN=%g BLUE=%g WHITE=%g SYNC=0 TRANSMIT=1", led_short(s->led),
             s->col[0], s->col[1], s->col[2], s->col[3]);
}
static void set_scales(CP* s, const float* c) {
    for (int i = 0; i < 4; i++) { s->col[i] = c[i]; if (s->sl[i]) lv_slider_set_value(s->sl[i], (int)(c[i] * 255), LV_ANIM_OFF); }
    update_preview(s);
}
static void on_released(lv_event_t*) { if (g_cp) { update_preview(g_cp); send_color(g_cp); } }
static void on_changed(lv_event_t*) { if (g_cp) update_preview(g_cp); }
static void apply_preset(lv_event_t* e) {
    CP* s = g_cp; if (!s) return;
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    float c[4];
    if (i >= 0) memcpy(c, s->pre[i].c, sizeof c);
    else { memset(c, 0, sizeof c); c[-i - 1] = 1; }                     // i < 0: channel swatch -> that channel at full
    set_scales(s, c);
    send_color(s);
}

static void add_preset(CP* s, const char* name, const float* c) {
    if (s->np >= 16) return;
    Preset& p = s->pre[s->np];
    strlcpy(p.name, name, sizeof p.name);
    memcpy(p.c, c, sizeof p.c);
    lv_color_t col = rgbw_to_rgb(c);
    lv_obj_t* b = ks_button_sized(s->plist, nullptr, 0, nullptr, 0, apply_preset, (void*)(intptr_t)s->np);
    lv_obj_set_size(b, 96, 64);
    swatch_style(b, col);
    lv_obj_t* l = ks_label(b, name, &ks_font_18);
    lv_obj_set_style_text_color(l, lv_color_brightness(col) > 128 ? lv_color_black() : lv_color_white(), 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_width(l, 88);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    s->np++;
}
static void on_presets(bool ok, JsonDocument& doc, void* u) {
    uint32_t gen = *(uint32_t*)u; delete (uint32_t*)u;
    CP* s = g_cp;
    if (!s || gen != g_gen_cp || !ok) return;
    // result.value = {entryId: {presets: {presetId: {name, red, green, blue, white}}}}: the first entry wins, like the Python
    int i = 0;
    for (JsonPairConst entry : doc["result"]["value"].as<JsonObjectConst>()) {
        for (JsonPairConst pr : entry.value()["presets"].as<JsonObjectConst>()) {
            float c[4]; const char* k[4] = {"red", "green", "blue", "white"};
            for (int j = 0; j < 4; j++) c[j] = pr.value()[k[j]].is<float>() ? pr.value()[k[j]].as<float>() / 255.0f : 0;
            const char* nm = pr.value()["name"] | "";
            char name[20], low[20];
            if (!*nm) snprintf(name, sizeof name, "%d", i);
            else { strlcpy(low, nm, sizeof low); for (char* p = low; *p; p++) *p = tolower((unsigned char)*p); strlcpy(name, low, sizeof name); }
            add_preset(s, name, c);
            i++;
        }
        break;
    }
}

struct ColorQ { const char* led; float c[4]; bool ok; };
static void read_color(JsonObjectConst root, void* u) {
    ColorQ* q = (ColorQ*)u;
    JsonArrayConst cd = root[q->led]["color_data"][0].as<JsonArrayConst>();      // first LED of the chain
    for (int i = 0; i < 4; i++) q->c[i] = cd[i] | 0.0f;
    q->ok = cd.size() >= 3;
}
static void on_state(lv_event_t*) {                              // process_update(): sliders follow color_data
    CP* s = g_cp; if (!s) return;
    for (int i = 0; i < 4; i++) if (s->sl[i] && lv_obj_has_state(s->sl[i], LV_STATE_PRESSED)) return;
    ColorQ q = {s->led, {0, 0, 0, 0}, false};
    ks_with_status(read_color, &q);
    if (!q.ok) return;
    bool same = true;
    for (int i = 0; i < 4; i++) if (fabsf(q.c[i] - s->col[i]) > 0.004f) same = false;
    if (!same) set_scales(s, q.c);
}

static void cp_del(lv_event_t*) { g_gen_cp++; delete g_cp; g_cp = nullptr; }

// colour page for g_open_led (panel body of the sub-page, or embedded when there is a single LED)
static void color_page(lv_obj_t* c) {
    g_gen_cp++;
    delete g_cp;
    g_cp = new CP();
    CP* s = g_cp;
    memset(s, 0, sizeof *s);
    strlcpy(s->led, g_open_led, sizeof s->led);
    strlcpy(s->order, "", sizeof s->order);
    for (int i = 0; i < g_nord; i++) if (!strcmp(g_ord[i].led, s->led)) strlcpy(s->order, g_ord[i].order, sizeof s->order);
    for (char* p = s->order; *p; p++) *p = toupper((unsigned char)*p);
    s->root = ks_card(c, lv_pct(100), lv_pct(100));
    lv_obj_add_event_cb(s->root, cp_del, LV_EVENT_DELETE, nullptr);
    lv_obj_set_flex_flow(s->root, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(s->root, 10, 0);

    if (!s->order[0]) {                                                  // get_led_color_order() returned None/empty
        lv_obj_center(ks_label(s->root, "Could not read the LED colour order", &ks_font_28));
        return;
    }
    ColorQ q = {s->led, {0, 0, 0, 0}, false};
    ks_with_status(read_color, &q);
    memcpy(s->col, q.c, sizeof s->col);

    lv_obj_t* left = ks_card(s->root, 380, lv_pct(100));              // channel rows
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(left, 8, 0);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    static const uint32_t CH[4] = {0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF};
    for (int i = 0; i < 4; i++) {
        if (!avail(s, i)) continue;
        lv_obj_t* row = ks_card(left, lv_pct(100), 80);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 10, 0);
        lv_obj_t* b = ks_button_sized(row, nullptr, 0, nullptr, 0, apply_preset, (void*)(intptr_t)(-i - 1));
        lv_obj_set_size(b, 72, 72);
        swatch_style(b, lv_color_hex(CH[i]));
        s->sw[i] = b;
        s->sl[i] = ks_slider(row, 0, 255, (int)(s->col[i] * 255), 270, on_released, nullptr);
        lv_obj_add_event_cb(s->sl[i], on_changed, LV_EVENT_VALUE_CHANGED, nullptr);
    }

    lv_obj_t* right = ks_card(s->root, 300, lv_pct(100));              // preview + presets
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(right, 6, 0);
    lv_obj_t* head = ks_card(right, lv_pct(100), 96);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s->hex = ks_label(head, "", &ks_font_28);
    s->prev = lv_obj_create(head);
    lv_obj_set_size(s->prev, 120, 90);
    lv_obj_clear_flag(s->prev, LV_OBJ_FLAG_SCROLLABLE);
    swatch_style(s->prev, lv_color_black());
    s->plist = ks_card(right, lv_pct(100), 300);
    lv_obj_set_flex_flow(s->plist, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(s->plist, 6, 0);
    lv_obj_add_flag(s->plist, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s->plist, LV_DIR_VER);
    float off[4] = {0, 0, 0, 0}, on[4];
    for (int i = 0; i < 4; i++) on[i] = avail(s, i) ? 1 : 0;
    add_preset(s, "off", off);
    add_preset(s, "on", on);
    update_preview(s);
    ks_on_state(s->root, on_state);
    ks_get_async("/server/database/item?namespace=mainsail&key=miscellaneous.entries", on_presets, new uint32_t(g_gen_cp));
}

// ---- LED selector ------------------------------------------------------------------------------------------------------------------
static void color_init(lv_obj_t* c) { color_page(c); }
static void pick(lv_event_t* e) {
    char names[MAXLED][40];
    int n = list_leds(names), i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i >= n) return;
    strlcpy(g_open_led, names[i], sizeof g_open_led);
    static char t[44];                                                     // title must outlive the call: ks_open keeps the pointer
    strlcpy(t, led_short(names[i]), sizeof t);
    ks_open(t, color_init);
}

struct Sel { lv_obj_t* root; };
static Sel* g_sel;
static void sel_del(lv_event_t*) { g_gen++; delete g_sel; g_sel = nullptr; }

static void on_cfg(JsonObjectConst cfg, bool ok, void*) {
    if (!g_sel) return;
    char names[MAXLED][40];
    int n = list_leds(names);
    g_nord = 0;
    for (int i = 0; i < n; i++) {                                            // colour order: explicit, else the channels that have pins / initial values
        char sect[40]; strlcpy(sect, names[i], sizeof sect);
        for (char* p = sect; *p; p++) *p = tolower((unsigned char)*p);
        Ord& o = g_ord[g_nord++];
        strlcpy(o.led, names[i], sizeof o.led);
        o.order[0] = 0;
        const char* co = ok ? kc_val(cfg, sect, "color_order") : nullptr;
        if (co) strlcpy(o.order, co, sizeof o.order);
        else if (ok) {
            static const char* const K[4][2] = {{"red_pin", "initial_red"}, {"green_pin", "initial_green"}, {"blue_pin", "initial_blue"}, {"white_pin", "initial_white"}};
            for (int c = 0; c < 4; c++) if ((kc_val(cfg, sect, K[c][0]) || kc_val(cfg, sect, K[c][1])) && !strchr(o.order, "RGBW"[c])) { size_t l = strlen(o.order); o.order[l] = "RGBW"[c]; o.order[l + 1] = 0; }
        }
    }
    lv_obj_clean(g_sel->root);
    if (n == 0) { lv_obj_center(ks_label(g_sel->root, "No LEDs", &ks_font_28)); return; }
    if (n == 1) { strlcpy(g_open_led, names[0], sizeof g_open_led); color_page(g_sel->root); return; }
    lv_obj_t* l = ks_list(g_sel->root);
    lv_obj_set_flex_flow(l, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(l, 4, 0);
    for (int i = 0; i < n; i++) {
        char up[40]; strlcpy(up, led_short(names[i]), sizeof up);
        for (char* p = up; *p; p++) *p = toupper((unsigned char)*p);
        lv_obj_t* b = ks_button_sized(l, nullptr, 0, up, (i % 4) + 1, pick, (void*)(intptr_t)i);
        lv_obj_set_size(b, 172, 130);
    }
}

void ks_panel_led(lv_obj_t* c) {
    g_gen++;
    delete g_sel;
    g_sel = new Sel();
    g_sel->root = ks_card(c, lv_pct(100), lv_pct(100));
    lv_obj_add_event_cb(g_sel->root, sel_del, LV_EVENT_DELETE, nullptr);
    lv_obj_center(ks_label(g_sel->root, "Loading...", &ks_font_28));
    static const char* const keys[] = {"color_order", "red_pin", "green_pin", "blue_pin", "white_pin",
                                       "initial_red", "initial_green", "initial_blue", "initial_white"};
    kc_fetch(&g_gen, nullptr, 0, keys, 9, on_cfg, nullptr);
}

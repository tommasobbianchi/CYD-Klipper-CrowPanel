// panels/power.py: one switch per Moonraker power device. GET /machine/device_power/devices (re-read every 3 s, like activate()),
// POST /machine/device_power/device?device=<name>&action=on|off. Devices flagged locked_while_printing ask for confirmation first.
// Needs the [power ...] sections of moonraker.conf: without them the endpoint answers 404 and the panel says so.
#include <Arduino.h>
#include "ks_widgets.h"
#include "ks_mr.h"
#include "ks_panels.h"

#define MAXDEV 16
struct Dev { char name[48]; bool locked; lv_obj_t* sw; uint32_t hold; };
struct PW { lv_obj_t* root; lv_obj_t* list; Dev d[MAXDEV]; int n; lv_timer_t* timer; char sig[MAXDEV * 48]; };
static PW* g_st;
static uint32_t g_gen;

static void url_enc(const char* in, char* out, size_t n) {
    size_t o = 0;
    for (; *in && o + 4 < n; in++) {
        if (isalnum((unsigned char)*in) || *in == '_' || *in == '-' || *in == '.') out[o++] = *in;
        else o += snprintf(out + o, n - o, "%%%02X", (unsigned char)*in);
    }
    out[o] = 0;
}

static void rebuild(PW* s, JsonArrayConst devs);
static void refresh(PW* s);

static void on_reply(bool ok, JsonDocument&, void* u) {
    uint32_t gen = *(uint32_t*)u; delete (uint32_t*)u;
    if (!ok && g_st && gen == g_gen) { ks_toast("Power device command failed"); refresh(g_st); }
}
static void send(PW* s, int i, bool on) {
    char enc[160], path[256];
    url_enc(s->d[i].name, enc, sizeof enc);
    snprintf(path, sizeof path, "/machine/device_power/device?device=%s&action=%s", enc, on ? "on" : "off");
    s->d[i].hold = millis() + 2500;
    ks_post_async(path, "{}", on_reply, new uint32_t(g_gen));
}
static void confirm_ok(void* u) {
    PW* s = g_st; if (!s) return;
    int i = (int)(intptr_t)u & 0xFF; bool on = ((intptr_t)u >> 8) & 1;
    if (i >= s->n) return;
    if (on) lv_obj_add_state(s->d[i].sw, LV_STATE_CHECKED); else lv_obj_clear_state(s->d[i].sw, LV_STATE_CHECKED);
    send(s, i, on);
}
static void on_switch(lv_event_t* e) {                                // on_switch(): power_device_on / power_device_off
    PW* s = g_st; if (!s) return;
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    lv_obj_t* sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (s->d[i].locked) {
        if (on) lv_obj_clear_state(sw, LV_STATE_CHECKED); else lv_obj_add_state(sw, LV_STATE_CHECKED);   // undo until confirmed
        char t[96]; snprintf(t, sizeof t, "Turn %s %s?", s->d[i].name, on ? "on" : "off");
        ks_confirm(t, "This device is locked while printing: Moonraker refuses the command during a print.", on ? "Turn on" : "Turn off",
                   confirm_ok, (void*)(intptr_t)(i | (on ? 0x100 : 0)));
        return;
    }
    send(s, i, on);
}

static void rebuild(PW* s, JsonArrayConst devs) {
    lv_obj_clean(s->list);
    s->n = 0;
    for (JsonObjectConst d : devs) {
        if (s->n >= MAXDEV) break;
        Dev& v = s->d[s->n];
        strlcpy(v.name, d["device"] | "?", sizeof v.name);
        v.locked = d["locked_while_printing"] | false;
        v.hold = 0;
        s->n++;
    }
    for (int i = 1; i < s->n; i++) for (int j = i; j > 0 && strcmp(s->d[j].name, s->d[j - 1].name) < 0; j--) { Dev t = s->d[j]; s->d[j] = s->d[j - 1]; s->d[j - 1] = t; }
    for (int i = 0; i < s->n; i++) {
        lv_obj_t* r = ks_row(s->list, s->d[i].name, s->d[i].locked ? "Locked while printing" : nullptr);
        s->d[i].sw = ks_switch(ks_row_right(r, 120), false, on_switch, (void*)(intptr_t)i);
    }
}

static void on_devices(bool ok, JsonDocument& doc, void* u) {
    uint32_t gen = *(uint32_t*)u; delete (uint32_t*)u;
    PW* s = g_st;
    if (!s || gen != g_gen) return;
    if (!ok) {
        if (!s->n && strcmp(s->sig, "!err")) {
            strlcpy(s->sig, "!err", sizeof s->sig); lv_obj_clean(s->list); lv_obj_t* l = ks_label(s->list, "No power devices (Moonraker has no [power] sections)", &ks_font_22); lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP); lv_obj_set_width(l, lv_pct(100)); }
        return;
    }
    JsonArrayConst devs = doc["result"]["devices"].as<JsonArrayConst>();
    char sig[sizeof s->sig] = "";
    for (JsonObjectConst d : devs) { strlcat(sig, d["device"] | "?", sizeof sig); strlcat(sig, d["locked_while_printing"] | false ? "!," : ",", sizeof sig); }
    if (strcmp(sig, s->sig)) { strlcpy(s->sig, sig, sizeof s->sig); rebuild(s, devs); }
    for (JsonObjectConst d : devs) {
        const char* nm = d["device"] | "";
        for (int i = 0; i < s->n; i++) {
            if (strcmp(s->d[i].name, nm) || (int32_t)(millis() - s->d[i].hold) < 0) continue;
            if (!strcmp(d["status"] | "off", "on")) lv_obj_add_state(s->d[i].sw, LV_STATE_CHECKED);
            else lv_obj_clear_state(s->d[i].sw, LV_STATE_CHECKED);
        }
    }
}
static void refresh(PW* s) { (void)s; ks_get_async("/machine/device_power/devices", on_devices, new uint32_t(g_gen)); }
static void on_timer(lv_timer_t*) { if (g_st) refresh(g_st); }

static void panel_del(lv_event_t*) {
    g_gen++;
    if (g_st && g_st->timer) lv_timer_del(g_st->timer);
    delete g_st; g_st = nullptr;
}

void ks_panel_power(lv_obj_t* c) {
    g_gen++;
    delete g_st;
    g_st = new PW();
    memset(g_st, 0, sizeof *g_st);
    g_st->root = ks_card(c, lv_pct(100), lv_pct(100));
    lv_obj_add_event_cb(g_st->root, panel_del, LV_EVENT_DELETE, nullptr);
    g_st->list = ks_list(g_st->root);
    g_st->timer = lv_timer_create(on_timer, 3000, nullptr);
    refresh(g_st);
}

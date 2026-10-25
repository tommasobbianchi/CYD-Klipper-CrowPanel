// panels/console.py: scrolling gcode log (GET /server/gcode_store), Auto-scroll / Hide temp. / Clear options, text entry + Send.
// Differences from the Python: the log is polled every 2 s (no websocket), and the entry sits ABOVE the log so the
// on-screen keyboard (bottom 250 px) never covers it.
#include <Arduino.h>
#include <string>
#include <time.h>
#include <ctype.h>
#include "ks_widgets.h"
#include "ks_mr.h"
#include "ks_panels.h"

namespace {

// panel colours of console.py COLORS
const char* C_COMMAND = "bad8ff";
const char* C_ERROR = "ff6975";
const char* C_RESPONSE = "b8b8b8";
const char* C_WARNING = "c9c9c9";
const char* C_TIME = "888888";

const int MAX_LINES = 120;

struct Con {
    lv_obj_t *log, *label, *entry, *b_auto, *b_temp;
    lv_timer_t* timer;
    std::string text;            // recolor markup, one line per entry
    int lines = 0;
    double last_time = 0;
    bool autoscroll = true, hidetemps = true, busy = false, first = true;
};

// ^(?:ok\s+)?(B|C|T\d*):
bool is_temp_line(const char* m) {
    if (!strncmp(m, "ok", 2) && isspace((unsigned char)m[2])) { m += 2; while (isspace((unsigned char)*m)) m++; }
    if (*m == 'B' || *m == 'C') return m[1] == ':';
    if (*m == 'T') { m++; while (isdigit((unsigned char)*m)) m++; return *m == ':'; }
    return false;
}

void drop_first_line(Con* c) {
    size_t nl = c->text.find('\n');
    c->text.erase(0, nl == std::string::npos ? c->text.size() : nl + 1);
    c->lines--;
}

void add_line(Con* c, const char* type, double t, const char* msg) {
    const char* color;
    std::string m;
    if (!strcmp(type, "command")) color = C_COMMAND;
    else if (!strncmp(msg, "!!", 2)) { color = C_ERROR; msg += (msg[2] == ' ') ? 3 : 2; }
    else if (!strncmp(msg, "//", 2)) { color = C_WARNING; msg += (msg[2] == ' ') ? 3 : 2; }
    else if (c->hidetemps && is_temp_line(msg)) return;
    else color = C_RESPONSE;
    for (const char* p = msg; *p; p++) {
        if (*p == '#') m += '*';                       // '#' would start/stop an LVGL recolor span
        else if (*p == '\n') m += "\n         ";
        else m += *p;
    }
    time_t tt = (time_t)t; struct tm lt; localtime_r(&tt, &lt);
    char ts[12]; strftime(ts, sizeof ts, "%H:%M:%S", &lt);
    if (c->lines) c->text += '\n';
    c->text += std::string("#") + C_TIME + " " + ts + "# #" + color + " " + m + "#";
    c->lines++;
    while (c->lines > MAX_LINES) drop_first_line(c);
}

void refresh_view(Con* c) {
    lv_label_set_text(c->label, c->text.empty() ? "" : c->text.c_str());
    if (c->autoscroll) { lv_obj_update_layout(c->log); lv_obj_scroll_to_y(c->log, LV_COORD_MAX, LV_ANIM_OFF); }
}

}  // namespace

// The callback needs to know the panel is still alive: Con is only freed in the DELETE handler, and results are only
// delivered while LVGL runs, so a registry of live panels is enough.
static Con* g_live = nullptr;
static void on_store_cb(bool ok, JsonDocument& doc, void* user) {
    Con* c = (Con*)user;
    if (c != g_live) return;                            // panel closed (or reopened) meanwhile
    c->busy = false;
    if (!ok) return;
    bool changed = false;
    double newest = c->last_time;
    for (JsonObjectConst e : doc["result"]["gcode_store"].as<JsonArrayConst>()) {
        double t = e["time"] | 0.0;
        if (t <= c->last_time) continue;
        add_line(c, e["type"] | "response", t, e["message"] | "");
        if (t > newest) newest = t;
        changed = true;
    }
    c->last_time = newest;
    if (changed || c->first) refresh_view(c);
    c->first = false;
}

static void poll(Con* c) {
    if (c->busy) return;
    c->busy = true;
    ks_get_async(c->first ? "/server/gcode_store?count=100" : "/server/gcode_store?count=40", on_store_cb, c);
}

static void send(lv_event_t* e) {
    Con* c = (Con*)lv_event_get_user_data(e);
    const char* cmd = lv_textarea_get_text(c->entry);
    if (!cmd || !*cmd) return;
    ks_gcode("%s", cmd);
    lv_textarea_set_text(c->entry, "");
    ks_keyboard_hide();
    c->busy = false;
    poll(c);                                            // the command shows up as a "command" entry of the store
}

static void paint(lv_obj_t* b, bool on) {               // toggle_active_class(button_active)
    lv_obj_set_style_bg_color(b, on ? KS_ACTIVE : KS_BG, 0);
    lv_obj_set_style_border_side(b, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(b, on ? 5 : 0, 0);
    lv_obj_set_style_border_color(b, ks_color(1), 0);
}
static void opt_auto(lv_event_t* e) { Con* c = (Con*)lv_event_get_user_data(e); c->autoscroll = !c->autoscroll; paint(c->b_auto, c->autoscroll); if (c->autoscroll) refresh_view(c); }
static void opt_temp(lv_event_t* e) { Con* c = (Con*)lv_event_get_user_data(e); c->hidetemps = !c->hidetemps; paint(c->b_temp, c->hidetemps); }
static void opt_clear(lv_event_t* e) {
    Con* c = (Con*)lv_event_get_user_data(e);
    c->text.clear(); c->lines = 0;                      // later polls only add entries newer than the last one shown
    refresh_view(c);
}

static lv_obj_t* opt_button(lv_obj_t* bar, const char* text, bool active, bool is_toggle, lv_event_cb_t cb, Con* c) {
    lv_obj_t* b = ks_button(bar, nullptr, nullptr, 0, cb, c);
    lv_obj_set_flex_grow(b, 1);
    lv_obj_set_height(b, lv_pct(100));
    ks_label(b, text, &ks_font_22);
    if (is_toggle) paint(b, active);
    return b;
}

void ks_panel_console(lv_obj_t* content) {
    Con* c = new Con();
    g_live = c;
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(content, 2, 0);
    lv_obj_set_style_pad_row(content, 4, 0);

    lv_obj_t* ebox = ks_card(content, lv_pct(100), 60);          // entry + Send
    lv_obj_set_flex_flow(ebox, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(ebox, 6, 0);
    c->entry = ks_textarea(ebox, "gcode", 520);
    lv_obj_set_flex_grow(c->entry, 1);
    lv_obj_set_height(c->entry, 60);
    lv_obj_add_event_cb(c->entry, send, LV_EVENT_READY, c);       // keyboard OK = "activate"
    lv_obj_t* enter = ks_button_sized(ebox, "resume", 40, nullptr, 3, send, c);
    lv_obj_set_size(enter, 120, 60);

    lv_obj_t* bar = ks_card(content, lv_pct(100), 56);           // options
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(bar, 4, 0);
    c->b_auto = opt_button(bar, "Auto-scroll", c->autoscroll, true, opt_auto, c);
    c->b_temp = opt_button(bar, "Hide temp.", c->hidetemps, true, opt_temp, c);
    opt_button(bar, "Clear", false, false, opt_clear, c);

    c->log = lv_obj_create(content);
    lv_obj_set_width(c->log, lv_pct(100));
    lv_obj_set_flex_grow(c->log, 1);
    lv_obj_set_style_bg_opa(c->log, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(c->log, 0, 0);
    lv_obj_set_style_pad_all(c->log, 4, 0);
    lv_obj_set_scroll_dir(c->log, LV_DIR_VER);
    c->label = ks_label(c->log, "", &ks_font_18);
    lv_label_set_recolor(c->label, true);
    lv_label_set_long_mode(c->label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(c->label, lv_pct(100));
    // the user scrolling by hand switches auto-scroll off, as in a text view you drag away from the end
    lv_obj_add_event_cb(c->log, [](lv_event_t* e) {
        Con* cc = (Con*)lv_event_get_user_data(e);
        if (cc->autoscroll && lv_obj_get_scroll_bottom(cc->log) > 40) { cc->autoscroll = false; paint(cc->b_auto, false); }
    }, LV_EVENT_SCROLL_END, c);

    c->timer = lv_timer_create([](lv_timer_t* t) { poll((Con*)t->user_data); }, 2000, c);
    lv_obj_add_event_cb(ebox, [](lv_event_t* e) {            // content itself is only cleaned, children are deleted
        Con* cc = (Con*)lv_event_get_user_data(e);
        lv_timer_del(cc->timer);
        if (g_live == cc) g_live = nullptr;
        delete cc;
    }, LV_EVENT_DELETE, c);
    poll(c);
}

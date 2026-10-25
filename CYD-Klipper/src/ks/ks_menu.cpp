// Main / More / Print-control menus: [menu __main ...] and [menu __print ...] from ks_includes/defaults.conf.
#include "ks_ui.h"
#include "ks_mr.h"
#include "ks_panels.h"

// Moonraker-side capabilities (printer.power_devices / cameras / spoolman), probed once per connection
static int g_power = 0, g_cams = 0;
static bool g_spoolman = false, g_caps_asked = false;

static void cb_power(bool ok, JsonDocument& d, void*) { if (ok) g_power = d["result"]["devices"].size(); }
static void cb_cams(bool ok, JsonDocument& d, void*) { if (ok) g_cams = d["result"]["webcams"].size(); }
static void cb_info(bool ok, JsonDocument& d, void*) {
    if (!ok) return;
    for (JsonVariantConst c : d["result"]["components"].as<JsonArrayConst>())
        if (!strcmp(c.as<const char*>() ? c.as<const char*>() : "", "spoolman")) g_spoolman = true;
}
static void on_conn(lv_event_t*) {
    if (!ks_connected()) { g_caps_asked = false; return; }
    if (g_caps_asked) return;
    g_caps_asked = true;
    ks_get_async("/machine/device_power/devices", cb_power, nullptr);
    ks_get_async("/server/webcams/list", cb_cams, nullptr);
    ks_get_async("/server/info", cb_info, nullptr);
}
void ks_menu_caps_init() {
    static lv_obj_t* anchor = lv_obj_create(lv_layer_top());   // invisible subscriber that lives forever
    lv_obj_set_size(anchor, 0, 0);
    lv_obj_add_flag(anchor, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(anchor, on_conn, LV_EVENT_MSG_RECEIVED, nullptr);
    lv_msg_subscribe_obj(KS_MSG_STATE, anchor, nullptr);
}

// ---- enable predicates (the {{ printer.* }} expressions of defaults.conf) ----------------------
static bool en_always() { return true; }
static bool en_heaters() { char n[2][40]; return ks_has("heater_bed") || ks_has("extruder") || ks_objects("temperature_sensor ", n, 1) > 0 || ks_objects("heater_generic ", n, 1) > 0; }
static bool en_extruder() { return ks_has("extruder"); }
static bool en_bedlevel() { return ks_has("bed_screws") || ks_has("screws_tilt_adjust"); }
static bool en_bedmesh() { return ks_has("bed_mesh"); }
static bool en_retraction() { return ks_has("firmware_retraction"); }
static bool en_fans() {
    char n[2][40];
    return ks_has("fan") || ks_objects("heater_fan ", n, 1) || ks_objects("controller_fan ", n, 1) ||
           ks_objects("fan_generic ", n, 1) || ks_objects("temperature_fan ", n, 1);
}
static bool en_leds() { char n[2][40]; return ks_objects("led ", n, 1) || ks_objects("neopixel ", n, 1) || ks_objects("dotstar ", n, 1) || ks_objects("pca9533 ", n, 1) || ks_objects("pca9632 ", n, 1); }
static bool en_macros() { char n[2][40]; return ks_macros(n, 1) > 0; }
static bool en_pins() { char n[2][40]; return ks_objects("output_pin ", n, 1) > 0; }
static bool en_power() { return g_power > 0; }
static bool en_camera() { return g_cams > 0; }
static bool en_shaper() { return ks_has("input_shaper"); }
static bool en_spoolman() { return g_spoolman; }
static bool en_paused() { return ks_b("pause_resume", "is_paused"); }

struct Item {
    const char* name; const char* icon; ks_panel_init panel; bool (*enable)();
    void (*action)();          // used instead of a panel (calibrateZoffset, save config)
};

// ---- actions ---------------------------------------------------------------------------------
static void act_calibrate_z() { ks_toast("calibrate Zoffset start!"); ks_gcode("_Delay_Calibrate"); }
static void do_save(void*) { ks_gcode("SAVE_CONFIG"); }
static void act_save() { ks_confirm("Save configuration?", "Klipper will reboot", "Save", do_save, nullptr); }

static const Item MORE[] = {
    {"Bed Level", "bed-level", ks_panel_bed_level, en_bedlevel, nullptr},
    {"Bed Mesh", "bed-mesh", ks_panel_bed_mesh, en_bedmesh, nullptr},
    {"Z Calibrate", "z-farther", ks_panel_zcalibrate, en_always, nullptr},
    {"Calibrate Zoffset", "z-farther", nullptr, en_always, act_calibrate_z},
    {"Limits", "fine-tune", ks_panel_limits, en_always, nullptr},
    {"Retraction", "retract", ks_panel_retraction, en_retraction, nullptr},
    {"Fan", "fan", ks_panel_fan, en_fans, nullptr},
    {"Leds", "light", ks_panel_led, en_leds, nullptr},
    {"Macros", "custom-script", ks_panel_macros, en_macros, nullptr},
    {"Pins", "hashtag", ks_panel_pins, en_pins, nullptr},
    {"Power", "shutdown", ks_panel_power, en_power, nullptr},
    {"Camera", "camera", ks_panel_camera, en_camera, nullptr},
    {"Console", "console", ks_panel_console, en_always, nullptr},
    {"System", "info", ks_panel_system, en_always, nullptr},
    {"Input Shaper", "move", ks_panel_input_shaper, en_shaper, nullptr},
    {"Save Config", "complete", nullptr, en_always, act_save},
    {"KlipperScreen", "settings", ks_panel_settings, en_always, nullptr},
    {"Network", "network", ks_panel_network, en_always, nullptr},
    {"Notifications", "notifications", ks_panel_notifications, en_always, nullptr},
    {"Spoolman", "spoolman", ks_panel_spoolman, en_spoolman, nullptr},
};

static const Item PRINT[] = {
    {"Temperature", "heat-up", ks_panel_temperature, en_heaters, nullptr},
    {"Fan", "fan", ks_panel_fan, en_fans, nullptr},
    {"Move", "move", ks_panel_move, en_paused, nullptr},
    {"Extrude", "extrude", ks_panel_extrude, en_extruder, nullptr},
    {"Power", "shutdown", ks_panel_power, en_power, nullptr},
    {"Leds", "light", ks_panel_led, en_leds, nullptr},
    {"Macros", "custom-script", ks_panel_macros, en_macros, nullptr},
    {"Camera", "camera", ks_panel_camera, en_camera, nullptr},
    {"Console", "console", ks_panel_console, en_always, nullptr},
    {"Limits", "fine-tune", ks_panel_limits, en_always, nullptr},
    {"Network", "network", ks_panel_network, en_always, nullptr},
    {"Retraction", "retract", ks_panel_retraction, en_retraction, nullptr},
    {"KlipperScreen", "settings", ks_panel_settings, en_always, nullptr},
    {"Spoolman", "spoolman", ks_panel_spoolman, en_spoolman, nullptr},
    {"Notifications", "notifications", ks_panel_notifications, en_always, nullptr},
};

static bool en_connected() { return ks_connected(); }
static const Item SPLASH[] = {
    {"Power", "shutdown", ks_panel_power, en_power, nullptr},
    {"Network", "network", ks_panel_network, en_always, nullptr},
    {"System", "info", ks_panel_system, en_connected, nullptr},
    {"KlipperScreen", "settings", ks_panel_settings, en_always, nullptr},
};
static void splash_menu_init(lv_obj_t* c);
static void more_init(lv_obj_t* c);
static void print_init(lv_obj_t* c);

static void item_clicked(lv_event_t* e) {
    const Item* it = (const Item*)lv_event_get_user_data(e);
    if (it->action) it->action();
    else if (it->panel) ks_open(it->name, it->panel);
}
static void more_clicked(lv_event_t*) { ks_open("More", more_init); }

// arrangeMenuItems(): 4 columns, but <=4 items -> 2 columns, <=6 -> 3 columns (menu.py)
static void build_grid(lv_obj_t* c, const Item* items, int n, const Item* extra_first = nullptr, lv_event_cb_t extra_cb = nullptr) {
    const Item* shown[40]; int cnt = 0;
    for (int i = 0; i < n && cnt < 40; i++) if (items[i].enable()) shown[cnt++] = &items[i];
    int total = cnt + (extra_first ? 1 : 0);
    int cols = total <= 4 ? 2 : total <= 6 ? 3 : 4;
    int rows = (total + cols - 1) / cols;
    if (rows < 2) rows = 2;

    static lv_coord_t colw[5], rowh[6];
    for (int i = 0; i < cols; i++) colw[i] = LV_GRID_FR(1);
    colw[cols] = LV_GRID_TEMPLATE_LAST;
    for (int i = 0; i < rows && i < 5; i++) rowh[i] = LV_GRID_FR(1);
    rowh[rows < 5 ? rows : 5] = LV_GRID_TEMPLATE_LAST;
    lv_obj_set_grid_dsc_array(c, colw, rowh);
    lv_obj_set_style_pad_all(c, 2, 0);
    lv_obj_set_style_pad_gap(c, 2, 0);

    int i = 0;
    for (int k = 0; k < cnt; k++, i++) {
        lv_obj_t* b = ks_button(c, shown[k]->icon, shown[k]->name, i % 4 + 1, item_clicked, (void*)shown[k]);
        lv_obj_set_grid_cell(b, LV_GRID_ALIGN_STRETCH, i % cols, 1, LV_GRID_ALIGN_STRETCH, i / cols, 1);
    }
    (void)extra_first; (void)extra_cb;
}

static void more_init(lv_obj_t* c) { build_grid(c, MORE, sizeof MORE / sizeof *MORE); }
static void print_init(lv_obj_t* c) { build_grid(c, PRINT, sizeof PRINT / sizeof *PRINT); }
static void splash_menu_init(lv_obj_t* c) { build_grid(c, SPLASH, sizeof SPLASH / sizeof *SPLASH); }
void ks_menu_open_splash() { ks_open("Menu", splash_menu_init); }
void ks_menu_open_print() { ks_open("Print Control", print_init); }

// [menu __main]: move, temperature, extrude, more, print
void ks_menu_open_main() {
    lv_obj_t* c = ks_content();
    struct Main { const char* name; const char* icon; ks_panel_init panel; bool (*en)(); bool more; };
    static const Main MAIN[] = {
        {"Move", "move", ks_panel_move, en_always, false},
        {"Temperature", "heat-up", ks_panel_temperature, en_heaters, false},
        {"Extrude", "extrude", ks_panel_extrude, en_extruder, false},
        {"More", "settings", nullptr, en_always, true},
        {"Print", "print", ks_panel_print, en_always, false},
    };
    static Item items[5];
    int n = 0;
    for (const Main& m : MAIN) if (m.en()) {
        items[n++] = {m.name, m.icon, m.panel, en_always, nullptr};
    }
    // 'More' opens a submenu, not a panel: route through more_init
    for (int i = 0; i < n; i++) if (!strcmp(items[i].name, "More")) items[i].panel = more_init;

    int cols = n <= 4 ? 2 : 3;
    static lv_coord_t colw[4], rowh[3];
    for (int i = 0; i < cols; i++) colw[i] = LV_GRID_FR(1);
    colw[cols] = LV_GRID_TEMPLATE_LAST;
    rowh[0] = rowh[1] = LV_GRID_FR(1); rowh[2] = LV_GRID_TEMPLATE_LAST;
    lv_obj_set_grid_dsc_array(c, colw, rowh);
    lv_obj_set_style_pad_all(c, 2, 0);
    lv_obj_set_style_pad_gap(c, 2, 0);
    for (int i = 0; i < n; i++) {
        lv_obj_t* b = ks_button(c, items[i].icon, items[i].name, i % 4 + 1, item_clicked, &items[i]);
        lv_obj_set_grid_cell(b, LV_GRID_ALIGN_STRETCH, i % cols, 1, LV_GRID_ALIGN_STRETCH, i / cols, 1);
    }
    (void)more_clicked;
}

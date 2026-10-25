#include "conf/global_config.h"
#include "core/screen_driver.h"
#include "ui/wifi_setup.h"
#include "ui/ip_setup.h"
#include "ui/serial/serial_console.h"
#include "lvgl.h"
#include "core/data_setup.h"
#include "ui/main_ui.h"
#include "ui/nav_buttons.h"
#include <Esp.h>
#include "core/lv_setup.h"
#include "ui/ota_setup.h"
#ifdef CYD_KS_UI
#include "ks/ks_ui.h"
#include "ks/ks_mr.h"
#include <ArduinoOTA.h>
#include <WiFi.h>
void ks_menu_caps_init();
void ks_debug_loop();
bool ks_wifi_held();
void ks_wifi_supervise();
#endif

void setup() {
    Serial.begin(115200);
    serial_console::greet();
    load_global_config();
    screen_setup();
    lv_setup();
    LOG_LN("Screen init done");
    
    wifi_init();
    ota_init();
    ip_init();
#ifdef CYD_KS_UI
    // Sovol KlipperScreen port: own Moonraker client + UI instead of the CYD printer abstraction
    ks_mr_start();
    ks_mr_tick_init();
    ks_menu_caps_init();
    ks_ui_start();
#else
    data_setup();

    nav_style_setup();
    main_ui_setup();
#endif
}

void loop(){
#ifdef CYD_KS_UI
    ks_wifi_supervise();              // non-blocking replacement for wifi_ok() (ks_debug.cpp)
#else
    wifi_ok();
#endif
#ifndef CYD_KS_UI
    data_loop();
#else
    // network flashing for development: espota.py -i <ip> -p 3232 -f firmware.bin (device connects back, so run it on the same LAN)
    static bool ota_started = false;
    if (!ota_started && WiFi.status() == WL_CONNECTED) {
        ArduinoOTA.setHostname("pandatouch");
        ArduinoOTA.begin();
        ota_started = true;
    }
    if (ota_started) { ArduinoOTA.handle(); ks_debug_loop(); }
#endif
    lv_handler();
    serial_console::run();

    if (is_ready_for_ota_update())
    {
        ota_do_update();
    }
}
// Weak placeholders: a panel that has no real implementation yet shows its name and says so.
// Each real panel file defines the strong symbol and replaces its stub at link time.
#include <stdio.h>
#include "ks_ui.h"
#include "ks_panels.h"

static void todo(lv_obj_t* c, const char* name) {
    char b[80];
    snprintf(b, sizeof b, "%s: not implemented yet", name);
    lv_obj_t* l = ks_label(c, b, &lv_font_montserrat_28);
    lv_obj_center(l);
}
#define STUB(fn, label) __attribute__((weak)) void fn(lv_obj_t* c) { todo(c, label); }
STUB(ks_panel_move, "Move")
STUB(ks_panel_temperature, "Temperature")
STUB(ks_panel_extrude, "Extrude")
STUB(ks_panel_print, "Print")
STUB(ks_panel_job_status, "Job status")
STUB(ks_panel_bed_level, "Bed Level")
STUB(ks_panel_bed_mesh, "Bed Mesh")
STUB(ks_panel_zcalibrate, "Z Calibrate")
STUB(ks_panel_limits, "Limits")
STUB(ks_panel_retraction, "Retraction")
STUB(ks_panel_fan, "Fan")
STUB(ks_panel_led, "Leds")
STUB(ks_panel_macros, "Macros")
STUB(ks_panel_pins, "Pins")
STUB(ks_panel_power, "Power")
STUB(ks_panel_camera, "Camera")
STUB(ks_panel_console, "Console")
STUB(ks_panel_system, "System")
STUB(ks_panel_input_shaper, "Input Shaper")
STUB(ks_panel_settings, "KlipperScreen settings")
STUB(ks_panel_network, "Network")
STUB(ks_panel_notifications, "Notifications")
STUB(ks_panel_spoolman, "Spoolman")

void ks_open_macros() { ks_open("Macros", ks_panel_macros); }

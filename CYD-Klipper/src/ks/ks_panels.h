// One init function per KlipperScreen panel (panels/*.py in Sovol's fork). Each fills `content`.
#pragma once
#include "lvgl.h"

void ks_panel_move(lv_obj_t* c);
void ks_panel_temperature(lv_obj_t* c);
void ks_panel_extrude(lv_obj_t* c);
void ks_panel_print(lv_obj_t* c);
void ks_panel_job_status(lv_obj_t* c);
void ks_panel_bed_level(lv_obj_t* c);
void ks_panel_bed_mesh(lv_obj_t* c);
void ks_panel_zcalibrate(lv_obj_t* c);
void ks_panel_limits(lv_obj_t* c);
void ks_panel_retraction(lv_obj_t* c);
void ks_panel_fan(lv_obj_t* c);
void ks_panel_led(lv_obj_t* c);
void ks_panel_macros(lv_obj_t* c);
void ks_panel_pins(lv_obj_t* c);
void ks_panel_power(lv_obj_t* c);
void ks_panel_camera(lv_obj_t* c);
void ks_panel_console(lv_obj_t* c);
void ks_panel_system(lv_obj_t* c);
void ks_panel_input_shaper(lv_obj_t* c);
void ks_panel_settings(lv_obj_t* c);
void ks_panel_network(lv_obj_t* c);
void ks_panel_notifications(lv_obj_t* c);
void ks_panel_spoolman(lv_obj_t* c);

void ks_panel_splash(lv_obj_t* c);
void ks_menu_open_splash();
void ks_open_macros();                       // action-bar shortcut
void ks_menu_open_main();
void ks_menu_open_print();                   // "Print Control" menu (job_status -> Control)
void ks_confirm(const char* title, const char* text, const char* ok_label, void (*on_ok)(void*), void* user);

#pragma once
#include "lvgl.h"
// KlipperScreen icon by name (e.g. "move"); smallest rendered size >= px, nullptr if unknown
const lv_img_dsc_t* ks_icon(const char* name, int px);

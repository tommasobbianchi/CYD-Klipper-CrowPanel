// LVGL allocator for the KlipperScreen build: replaces the fixed 40 KB pool (which ran out when several heavy panels
// were opened in a row) by small blocks in internal RAM while it has headroom, everything else in PSRAM.
#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
void* ks_lv_alloc(size_t size);
void ks_lv_free(void* p);
void* ks_lv_realloc(void* p, size_t size);
#ifdef __cplusplus
}
#endif

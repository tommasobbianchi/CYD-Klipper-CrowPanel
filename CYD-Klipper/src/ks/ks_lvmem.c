#include <string.h>
#include <esp_heap_caps.h>
#include "ks_lvmem.h"

#define SMALL_BLOCK 2048           /* object / style / label sized blocks stay fast, in internal RAM */
#define INTERNAL_FLOOR (48 * 1024) /* ... while WiFi and the HTTP client keep this much internal heap */

void* ks_lv_alloc(size_t size) {
    void* p = NULL;
    if (size <= SMALL_BLOCK && heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) > INTERNAL_FLOOR)
        p = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!p) p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p;
}
void ks_lv_free(void* p) { heap_caps_free(p); }
void* ks_lv_realloc(void* p, size_t size) {
    void* n = ks_lv_alloc(size);
    if (!n) return NULL;
    if (p) {
        size_t old = heap_caps_get_allocated_size(p);
        memcpy(n, p, old < size ? old : size);
        heap_caps_free(p);
    }
    return n;
}

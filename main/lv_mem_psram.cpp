/**
 * @file lv_mem_psram.cpp
 * @brief LVGL memory backend that keeps widgets in PSRAM.
 *
 * LVGL creates thousands of small objects (widgets, styles, draw tasks). With the
 * default C-library backend they land in internal SRAM, which on the Tab5 is the
 * scarce resource: WiFi over esp_hosted, LWIP, I2S/LCD DMA and every task stack
 * compete for the same ~500 KB. This backend (selected with
 * CONFIG_LV_USE_CUSTOM_MALLOC) serves LVGL from the 32 MB PSRAM instead, falling
 * back to internal memory only if PSRAM is exhausted. The P4 caches PSRAM, so the
 * UI does not get measurably slower.
 */
#include "lvgl.h"
#include "esp_heap_caps.h"

extern "C" {

void lv_mem_init(void) {}
void lv_mem_deinit(void) {}

lv_mem_pool_t lv_mem_add_pool(void *, size_t) { return nullptr; }
void lv_mem_remove_pool(lv_mem_pool_t) {}

void *lv_malloc_core(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void *lv_realloc_core(void *p, size_t new_size)
{
    void *q = heap_caps_realloc(p, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return q ? q : heap_caps_realloc(p, new_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void lv_free_core(void *p) { heap_caps_free(p); }

void lv_mem_monitor_core(lv_mem_monitor_t *mon)
{
    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_SPIRAM);
    mon->total_size = info.total_free_bytes + info.total_allocated_bytes;
    mon->free_size = info.total_free_bytes;
    mon->free_biggest_size = info.largest_free_block;
    mon->used_cnt = info.allocated_blocks;
    mon->free_cnt = info.free_blocks;
    mon->used_pct = mon->total_size ? 100 - (100 * mon->free_size) / mon->total_size : 0;
    mon->frag_pct = mon->free_size ? 100 - (100 * mon->free_biggest_size) / mon->free_size : 0;
}

lv_result_t lv_mem_test_core(void) { return heap_caps_check_integrity(MALLOC_CAP_SPIRAM, false) ? LV_RESULT_OK : LV_RESULT_INVALID; }

} // extern "C"

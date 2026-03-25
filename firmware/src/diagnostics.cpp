#include "diagnostics.h"
#include "config.h"
#include <Arduino.h>
#include <esp_system.h>

static uint32_t s_heap_before     = 0;
static uint32_t s_heap_min_seen   = UINT32_MAX;
static uint32_t s_poll_count      = 0;

// Log every N polls to avoid flooding the serial monitor.
static constexpr uint32_t LOG_EVERY_N_POLLS = 10;

void diag_heap_before_poll() {
    s_heap_before = esp_get_free_heap_size();
}

void diag_heap_after_poll() {
    s_poll_count++;

    uint32_t heap_after = esp_get_free_heap_size();
    uint32_t heap_min   = esp_get_minimum_free_heap_size();

    if (heap_min < s_heap_min_seen) {
        s_heap_min_seen = heap_min;
    }

    if (s_poll_count % LOG_EVERY_N_POLLS == 0) {
        Serial.printf("[Diag] poll#%lu  before=%lu  after=%lu  delta=%+ld  alltime_min=%lu\n",
                      (unsigned long)s_poll_count,
                      (unsigned long)s_heap_before,
                      (unsigned long)heap_after,
                      (long)heap_after - (long)s_heap_before,
                      (unsigned long)s_heap_min_seen);
    }
}

void diag_check_heap() {
    uint32_t free_heap = esp_get_free_heap_size();
    if (free_heap < HEAP_REBOOT_THRESHOLD) {
        Serial.printf("[Diag] CRITICAL: free heap %lu < threshold %d — rebooting!\n",
                      (unsigned long)free_heap, HEAP_REBOOT_THRESHOLD);
        delay(200);  // Allow serial flush.
        esp_restart();
    }
}

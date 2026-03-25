#include <Arduino.h>
#include "config.h"
#include "wifi_manager.h"
#include "telegram_bot.h"
#include "caldav_client.h"
#include "diagnostics.h"

static bool s_bot_started = false;

// ---------------------------------------------------------------------------
// One-time setup
// ---------------------------------------------------------------------------

void setup() {
    Serial.begin(115200);
    delay(500);  // Let serial settle.
    Serial.println("\n[Main] ESP32-C3 Calendar Bot — Phase 1 starting...");
    Serial.printf("[Main] Free heap at boot: %lu bytes\n",
                  (unsigned long)esp_get_free_heap_size());

    wifi_begin();

    // Wait for initial WiFi connection before initialising the bot.
    // This blocks for up to 15 seconds; if WiFi is not available, wifi_maintain()
    // will keep retrying in the main loop.
    uint32_t timeout = millis() + 15000;
    while (!wifi_is_connected() && millis() < timeout) {
        wifi_maintain();
        delay(100);
    }

    if (wifi_is_connected()) {
        caldav_begin();  // NTP sync + build Basic Auth header
        bot_begin();
        s_bot_started = true;
    } else {
        Serial.println("[Main] WiFi not available at boot; bot will start after connection.");
    }
}

// ---------------------------------------------------------------------------
// Main loop
// ---------------------------------------------------------------------------

void loop() {
    wifi_maintain();

    if (!wifi_is_connected()) return;

    // Start the bot the first time we have connectivity (handles the case where
    // WiFi wasn't available during setup()).
    if (!s_bot_started) {
        caldav_begin();
        bot_begin();
        s_bot_started = true;
    }

    bot_poll();
    diag_check_heap();
}

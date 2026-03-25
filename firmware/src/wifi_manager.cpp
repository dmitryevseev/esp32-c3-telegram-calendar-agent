#include "wifi_manager.h"
#include "config.h"
#include <Arduino.h>
#include <WiFi.h>

static uint32_t s_reconnect_delay_ms = WIFI_RECONNECT_INITIAL_DELAY_MS;
static uint32_t s_next_reconnect_at  = 0;
static bool     s_was_connected      = false;

// Update the status LED to reflect the current state.
static void led_set(bool on) {
#if STATUS_LED_PIN >= 0
    digitalWrite(STATUS_LED_PIN, on ? HIGH : LOW);
#endif
}

static void led_blink_fast() {
    static uint32_t last = 0;
    static bool     state = false;
    if (millis() - last >= 200) {
        last = millis();
        state = !state;
        led_set(state);
    }
}

static void led_blink_slow() {
    static uint32_t last = 0;
    static bool     state = false;
    if (millis() - last >= 1000) {
        last = millis();
        state = !state;
        led_set(state);
    }
}

void wifi_begin() {
#if STATUS_LED_PIN >= 0
    pinMode(STATUS_LED_PIN, OUTPUT);
#endif
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);  // We manage reconnection ourselves.
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.printf("[WiFi] Connecting to \"%s\"...\n", WIFI_SSID);
}

void wifi_maintain() {
    if (WiFi.status() == WL_CONNECTED) {
        if (!s_was_connected) {
            s_was_connected      = true;
            s_reconnect_delay_ms = WIFI_RECONNECT_INITIAL_DELAY_MS;
            Serial.printf("[WiFi] Connected. IP: %s  RSSI: %d dBm\n",
                          WiFi.localIP().toString().c_str(),
                          WiFi.RSSI());
        }
        led_blink_slow();
        return;
    }

    // Lost connection.
    if (s_was_connected) {
        s_was_connected = false;
        Serial.println("[WiFi] Disconnected.");
        led_set(false);
    }

    // Blink fast while waiting to reconnect.
    led_blink_fast();

    if (millis() < s_next_reconnect_at) {
        return;
    }

    Serial.printf("[WiFi] Reconnecting (delay was %lums)...\n", s_reconnect_delay_ms);
    WiFi.disconnect(true);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    // Exponential backoff, capped at max.
    s_next_reconnect_at   = millis() + s_reconnect_delay_ms;
    s_reconnect_delay_ms  = min(s_reconnect_delay_ms * 2,
                                (uint32_t)WIFI_RECONNECT_MAX_DELAY_MS);
}

bool wifi_is_connected() {
    return WiFi.status() == WL_CONNECTED;
}

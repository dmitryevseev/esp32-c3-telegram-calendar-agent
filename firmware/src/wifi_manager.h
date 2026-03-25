#pragma once

// WiFi connection manager with automatic reconnection.
//
// Usage:
//   wifi_begin();           // call once in setup()
//   wifi_maintain();        // call every loop() iteration
//   wifi_is_connected();    // check before making network requests

void wifi_begin();
void wifi_maintain();
bool wifi_is_connected();

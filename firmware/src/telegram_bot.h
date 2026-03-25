#pragma once

// Telegram bot: long-poll for updates, dispatch to handlers.
//
// Usage:
//   bot_begin();    // call once in setup(), after WiFi is connected
//   bot_poll();     // call in loop() when wifi_is_connected()

void bot_begin();
void bot_poll();

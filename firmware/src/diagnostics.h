#pragma once

// Heap diagnostics: log free RAM around each Telegram poll, reboot if critically low.
//
// Usage:
//   diag_heap_before_poll();   // call immediately before bot_poll's getUpdates
//   diag_heap_after_poll();    // call immediately after getUpdates returns
//   diag_check_heap();         // call in loop() to trigger reboot if heap < threshold

void diag_heap_before_poll();
void diag_heap_after_poll();
void diag_check_heap();

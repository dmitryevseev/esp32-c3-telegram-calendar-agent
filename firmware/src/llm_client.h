#pragma once

// ---------------------------------------------------------------------------
// LLM client — sends a user message to the Claude API and returns a
// structured tool call describing the user's intent.
//
// Usage:
//   char today[12];
//   // fill today as "YYYY-MM-DD"
//   LlmToolCall tc = llm_classify(user_text, event_context, today);
//   if (tc.valid) { /* dispatch on tc.tool_name */ }
// ---------------------------------------------------------------------------

// Fixed-size result struct — no heap strings, safe to return by value.
// Total size: ~380 bytes, fits comfortably on the stack.
struct LlmToolCall {
    char tool_name[24];        // "list_events" | "create_event" | "delete_event"
                               // | "reply" | "ping"
    char param_date_start[12]; // YYYY-MM-DD — used by list_events (start) and
                               // create_event (the event date)
    char param_date_end[12];   // YYYY-MM-DD — used by list_events (end)
    char param_time[6];        // HH:MM — used by create_event
    char param_title[64];      // used by create_event
    char param_message[256];   // used by reply; also carries error text when !valid
    int  param_index;          // 1-based event number — used by delete_event
    int  param_duration;       // minutes — used by create_event (default 60)
    bool valid;                // false when the call failed (see error_code)
    int  error_code;           // 0 = success; negative = local error;
                               // positive = HTTP status code
};

// Classify a user message via the Claude Messages API.
//
//   user_message   — the raw text sent by the user
//   event_context  — optional compact listing of cached events, e.g.
//                    "1. Meeting 09:00-10:00\n2. Lunch 12:00-13:00"
//                    Pass nullptr or "" if the cache is empty.
//   today_str      — current local date as "YYYY-MM-DD"
//
// Returns a populated LlmToolCall.  On error, valid == false and
// param_message contains a user-facing error string.
LlmToolCall llm_classify(const char* user_message,
                          const char* event_context,
                          const char* today_str);
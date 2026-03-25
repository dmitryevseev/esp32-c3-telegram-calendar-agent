#include "llm_client.h"
#include "config.h"
#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>

// ---------------------------------------------------------------------------
// Internal constants
// ---------------------------------------------------------------------------

// Minimum free heap required before attempting an LLM call.
// TLS handshake + request/response buffers need ~25KB; 40KB gives headroom.
static constexpr size_t k_min_heap  = 40000;

// Maximum bytes to read from the Claude API response body.
// With max_tokens=300 the JSON response is typically 600-900 bytes.
static constexpr size_t k_resp_size = 4096;

// ---------------------------------------------------------------------------
// Build the tools array into the request JSON document
// ---------------------------------------------------------------------------

static void build_tools(JsonArray tools) {
    // list_events --------------------------------------------------------
    {
        JsonObject t = tools.add<JsonObject>();
        t["name"]        = "list_events";
        t["description"] = "List calendar events in a date range";
        JsonObject schema = t["input_schema"].to<JsonObject>();
        schema["type"] = "object";
        JsonObject props = schema["properties"].to<JsonObject>();
        JsonObject p1 = props["start_date"].to<JsonObject>();
        p1["type"] = "string"; p1["description"] = "Start date YYYY-MM-DD";
        JsonObject p2 = props["end_date"].to<JsonObject>();
        p2["type"] = "string"; p2["description"] = "End date YYYY-MM-DD";
        JsonArray req = schema["required"].to<JsonArray>();
        req.add("start_date"); req.add("end_date");
    }
    // create_event -------------------------------------------------------
    {
        JsonObject t = tools.add<JsonObject>();
        t["name"]        = "create_event";
        t["description"] = "Create a new calendar event";
        JsonObject schema = t["input_schema"].to<JsonObject>();
        schema["type"] = "object";
        JsonObject props = schema["properties"].to<JsonObject>();
        JsonObject p1 = props["date"].to<JsonObject>();
        p1["type"] = "string"; p1["description"] = "Date YYYY-MM-DD";
        JsonObject p2 = props["time"].to<JsonObject>();
        p2["type"] = "string"; p2["description"] = "Time HH:MM 24-hour";
        JsonObject p3 = props["title"].to<JsonObject>();
        p3["type"] = "string"; p3["description"] = "Event title";
        JsonObject p4 = props["duration_minutes"].to<JsonObject>();
        p4["type"] = "integer"; p4["description"] = "Duration in minutes (default 60)";
        JsonArray req = schema["required"].to<JsonArray>();
        req.add("date"); req.add("time"); req.add("title");
    }
    // delete_event -------------------------------------------------------
    {
        JsonObject t = tools.add<JsonObject>();
        t["name"]        = "delete_event";
        t["description"] = "Delete an event by its number from the last listing";
        JsonObject schema = t["input_schema"].to<JsonObject>();
        schema["type"] = "object";
        JsonObject props = schema["properties"].to<JsonObject>();
        JsonObject p1 = props["event_index"].to<JsonObject>();
        p1["type"] = "integer"; p1["description"] = "1-based event number from last listing";
        JsonArray req = schema["required"].to<JsonArray>();
        req.add("event_index");
    }
    // reply --------------------------------------------------------------
    {
        JsonObject t = tools.add<JsonObject>();
        t["name"]        = "reply";
        t["description"] = "Send a text reply to the user (for clarification or chitchat)";
        JsonObject schema = t["input_schema"].to<JsonObject>();
        schema["type"] = "object";
        JsonObject props = schema["properties"].to<JsonObject>();
        JsonObject p1 = props["message"].to<JsonObject>();
        p1["type"] = "string"; p1["description"] = "Message to send";
        JsonArray req = schema["required"].to<JsonArray>();
        req.add("message");
    }
    // ping ---------------------------------------------------------------
    {
        JsonObject t = tools.add<JsonObject>();
        t["name"]        = "ping";
        t["description"] = "Health check, report device status";
        JsonObject schema = t["input_schema"].to<JsonObject>();
        schema["type"] = "object";
        schema["properties"].to<JsonObject>();
    }
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

LlmToolCall llm_classify(const char* user_message,
                          const char* event_context,
                          const char* today_str) {
    LlmToolCall result = {};  // zero-init: valid=false, error_code=0

    // --- Heap guard -------------------------------------------------------
    size_t free_heap = esp_get_free_heap_size();
    if (free_heap < k_min_heap) {
        Serial.printf("[LLM] Heap too low (%u bytes), skipping call\n",
                      (unsigned)free_heap);
        snprintf(result.param_message, sizeof(result.param_message),
                 "Device memory low. Use /help for slash commands.");
        result.error_code = -1;
        return result;
    }

    // --- System prompt ----------------------------------------------------
    char system_prompt[480];
    if (event_context && event_context[0] != '\0') {
        snprintf(system_prompt, sizeof(system_prompt),
            "You are a calendar assistant on an embedded device. "
            "Today is %s, timezone UTC%+d. "
            "Always resolve relative dates to absolute YYYY-MM-DD. "
            "Current cached events:\n%s",
            today_str,
            TIMEZONE_OFFSET_SEC / 3600,
            event_context);
    } else {
        snprintf(system_prompt, sizeof(system_prompt),
            "You are a calendar assistant on an embedded device. "
            "Today is %s, timezone UTC%+d. "
            "Always resolve relative dates to absolute YYYY-MM-DD.",
            today_str,
            TIMEZONE_OFFSET_SEC / 3600);
    }

    // --- Build request JSON (nested scope to free the doc before TLS) -----
    char* req_buf = nullptr;
    size_t req_len = 0;
    {
        JsonDocument req_doc;
        req_doc["model"]      = CLAUDE_MODEL;
        req_doc["max_tokens"] = 300;
        req_doc["system"]     = system_prompt;

        JsonArray tools = req_doc["tools"].to<JsonArray>();
        build_tools(tools);

        JsonArray messages    = req_doc["messages"].to<JsonArray>();
        JsonObject user_msg   = messages.add<JsonObject>();
        user_msg["role"]      = "user";
        user_msg["content"]   = user_message;

        req_len = measureJson(req_doc);
        req_buf = (char*)malloc(req_len + 1);
        if (!req_buf) {
            Serial.println("[LLM] OOM building request");
            snprintf(result.param_message, sizeof(result.param_message),
                     "Out of memory.");
            result.error_code = -2;
            return result;
        }
        serializeJson(req_doc, req_buf, req_len + 1);
        // req_doc destroyed here — frees its heap before TLS opens
    }

    Serial.printf("[LLM] POST %u bytes, heap %u\n",
                  (unsigned)req_len, (unsigned)esp_get_free_heap_size());

    // --- HTTP POST --------------------------------------------------------
    WiFiClientSecure tls;
    tls.setInsecure();

    HTTPClient http;
    http.setTimeout(CLAUDE_API_TIMEOUT_MS);
    http.begin(tls, "https://api.anthropic.com/v1/messages");
    http.addHeader("Content-Type",      "application/json");
    http.addHeader("x-api-key",         CLAUDE_API_KEY);
    http.addHeader("anthropic-version", "2023-06-01");

    int http_code = http.POST((uint8_t*)req_buf, (int)req_len);
    free(req_buf);
    req_buf = nullptr;

    // --- HTTP error handling ----------------------------------------------
    if (http_code <= 0) {
        Serial.printf("[LLM] Connection error: %d\n", http_code);
        snprintf(result.param_message, sizeof(result.param_message),
                 "Cannot reach AI service. Use /help for slash commands.");
        result.error_code = http_code;
        http.end();
        return result;
    }
    if (http_code == 401) {
        snprintf(result.param_message, sizeof(result.param_message),
                 "AI service: invalid API key. Check CLAUDE_API_KEY in config.h.");
        result.error_code = 401;
        http.end();
        return result;
    }
    if (http_code == 429) {
        snprintf(result.param_message, sizeof(result.param_message),
                 "AI service rate-limited. Try again in a moment.");
        result.error_code = 429;
        http.end();
        return result;
    }
    if (http_code != 200) {
        Serial.printf("[LLM] HTTP %d\n", http_code);
        snprintf(result.param_message, sizeof(result.param_message),
                 "AI service error (%d). Use /help for slash commands.", http_code);
        result.error_code = http_code;
        http.end();
        return result;
    }

    // --- Read response body -----------------------------------------------
    String resp_str = http.getString();
    http.end();

    Serial.printf("[LLM] Response %u bytes, heap %u\n",
                  (unsigned)resp_str.length(), (unsigned)esp_get_free_heap_size());

    if (resp_str.length() == 0) {
        snprintf(result.param_message, sizeof(result.param_message),
                 "Empty AI response. Use /help for slash commands.");
        result.error_code = -3;
        return result;
    }

    // --- Parse JSON -------------------------------------------------------
    JsonDocument resp_doc;
    DeserializationError err = deserializeJson(resp_doc, resp_str.c_str());
    resp_str = "";  // release String memory before we do more work

    if (err) {
        Serial.printf("[LLM] JSON parse error: %s\n", err.c_str());
        snprintf(result.param_message, sizeof(result.param_message),
                 "Could not parse AI response. Use /help for slash commands.");
        result.error_code = -4;
        return result;
    }

    // --- Extract tool_use block -------------------------------------------
    JsonArray content = resp_doc["content"];
    for (JsonObject item : content) {
        if (strcmp(item["type"] | "", "tool_use") != 0) continue;

        const char* name = item["name"] | "";
        strncpy(result.tool_name, name, sizeof(result.tool_name) - 1);

        JsonObject input = item["input"];

        // date_start: list_events uses "start_date"; create_event uses "date"
        const char* v_date_start = input["start_date"] | "";
        if (v_date_start[0] == '\0') v_date_start = input["date"] | "";
        strncpy(result.param_date_start, v_date_start, sizeof(result.param_date_start) - 1);

        strncpy(result.param_date_end, input["end_date"]  | "", sizeof(result.param_date_end)   - 1);
        strncpy(result.param_time,     input["time"]      | "", sizeof(result.param_time)       - 1);
        strncpy(result.param_title,    input["title"]     | "", sizeof(result.param_title)      - 1);
        strncpy(result.param_message,  input["message"]   | "", sizeof(result.param_message)    - 1);

        result.param_index    = input["event_index"]       | 0;
        result.param_duration = input["duration_minutes"]  | 60;

        result.valid = true;
        break;
    }

    if (!result.valid) {
        Serial.println("[LLM] No tool_use block in response");
        snprintf(result.param_message, sizeof(result.param_message),
                 "AI did not return an action. Use /help for slash commands.");
        result.error_code = -5;
    }

    return result;
}

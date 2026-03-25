#include "caldav_client.h"
#include "ical_builder.h"
#include "ical_parser.h"
#include "config.h"

#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <mbedtls/base64.h>
#include <esp_random.h>

// ---------------------------------------------------------------------------
// Module-level state
// ---------------------------------------------------------------------------

// Pre-built "Authorization: Basic <b64>" header value (built once in caldav_begin).
static char s_auth_header[256] = {};

// ---------------------------------------------------------------------------
// NTP
// ---------------------------------------------------------------------------

// Servers to try in order.
static const char* NTP_SERVERS[] = {
    "pool.ntp.org",
    "time.nist.gov",
    "time.google.com",
};

bool caldav_time_synced() {
    return time(nullptr) > 1000000000L;  // any plausible post-2001 timestamp
}

static void sync_ntp() {
    // Configure SNTP with three fallback servers.
    configTime(0, 0,
               NTP_SERVERS[0],
               NTP_SERVERS[1],
               NTP_SERVERS[2]);

    Serial.print("[CalDAV] Waiting for NTP sync");
    const uint32_t deadline = millis() + 5000;
    while (!caldav_time_synced() && millis() < deadline) {
        delay(200);
        Serial.print('.');
    }
    Serial.println();

    if (caldav_time_synced()) {
        struct tm ti;
        getLocalTime(&ti);
        Serial.printf("[CalDAV] NTP synced: %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                      ti.tm_year + 1900, ti.tm_mon + 1, ti.tm_mday,
                      ti.tm_hour, ti.tm_min, ti.tm_sec);
    } else {
        Serial.println("[CalDAV] NTP sync timed out — timestamps may be wrong");
    }
}

// ---------------------------------------------------------------------------
// Basic Auth header
// ---------------------------------------------------------------------------

static void build_auth_header() {
    // Plain-text credentials: "apple_id:app_password"
    char creds[192];
    snprintf(creds, sizeof(creds), "%s:%s", CALDAV_APPLE_ID, CALDAV_APP_PASSWORD);

    // Base64-encode.
    unsigned char b64[256];
    size_t b64_len = 0;
    mbedtls_base64_encode(b64, sizeof(b64), &b64_len,
                          (const unsigned char*)creds, strlen(creds));
    b64[b64_len] = '\0';

    snprintf(s_auth_header, sizeof(s_auth_header), "Basic %s", (char*)b64);
}

// ---------------------------------------------------------------------------
// Public init
// ---------------------------------------------------------------------------

void caldav_begin() {
    build_auth_header();
    sync_ntp();
}

// ---------------------------------------------------------------------------
// Low-level HTTP helper
// ---------------------------------------------------------------------------

// Execute a single CalDAV request and capture the response body.
//
//   method        — HTTP method string ("GET", "PUT", "DELETE", "REPORT", …)
//   url           — full HTTPS URL
//   content_type  — Content-Type header value (may be nullptr for body-less requests)
//   body          — request body (may be nullptr)
//   body_len      — length of body (0 if no body)
//   resp_buf      — caller-provided buffer for response body
//   resp_buf_len  — size of resp_buf
//
// Returns HTTP status code, or negative on connection error.
static int caldav_request(const char* method,
                          const char* url,
                          const char* content_type,
                          const char* body,
                          size_t body_len,
                          char* resp_buf,
                          size_t resp_buf_len) {
    WiFiClientSecure tls;
    tls.setInsecure();  // Accept any certificate — iCloud uses a well-known CA but
                        // maintaining a CA bundle on-device adds complexity.

    HTTPClient http;
    if (!http.begin(tls, url)) {
        Serial.printf("[CalDAV] http.begin() failed for %s\n", url);
        return -1;
    }

    http.addHeader("Authorization", s_auth_header);
    http.addHeader("User-Agent",    "ESP32CalBot/1.0");

    if (content_type && body_len > 0) {
        http.addHeader("Content-Type", content_type);
    }

    int code;
    if (body && body_len > 0) {
        code = http.sendRequest(method, (uint8_t*)body, body_len);
    } else {
        code = http.sendRequest(method);
    }

    Serial.printf("[CalDAV] %s %s → HTTP %d\n", method, url, code);

    if (resp_buf && resp_buf_len > 1 && code > 0) {
        String payload = http.getString();
        size_t copy = payload.length();
        if (copy >= resp_buf_len) copy = resp_buf_len - 1;
        memcpy(resp_buf, payload.c_str(), copy);
        resp_buf[copy] = '\0';
    }

    http.end();
    return code;
}

// ---------------------------------------------------------------------------
// REPORT XML template — fetch events in a time range
// ---------------------------------------------------------------------------

// iCal date-time format for CalDAV time-range filter: "YYYYMMDDTHHMMSSz"
static void fmt_ical_utc(char* buf, size_t len, time_t t) {
    struct tm* gm = gmtime(&t);
    strftime(buf, len, "%Y%m%dT%H%M%SZ", gm);
}

static const char REPORT_TEMPLATE[] =
    "<?xml version=\"1.0\" encoding=\"utf-8\" ?>"
    "<C:calendar-query xmlns:D=\"DAV:\" xmlns:C=\"urn:ietf:params:xml:ns:caldav\">"
      "<D:prop>"
        "<D:getetag/>"
        "<C:calendar-data/>"
      "</D:prop>"
      "<C:filter>"
        "<C:comp-filter name=\"VCALENDAR\">"
          "<C:comp-filter name=\"VEVENT\">"
            "<C:time-range start=\"%s\" end=\"%s\"/>"
          "</C:comp-filter>"
        "</C:comp-filter>"
      "</C:filter>"
    "</C:calendar-query>";

int caldav_list_events(time_t start_utc, time_t end_utc,
                       CalEvent* events, int max_events) {
    char start_str[18], end_str[18];
    fmt_ical_utc(start_str, sizeof(start_str), start_utc);
    fmt_ical_utc(end_str,   sizeof(end_str),   end_utc);

    // Build REPORT body.
    char body[512];
    int body_len = snprintf(body, sizeof(body), REPORT_TEMPLATE, start_str, end_str);
    if (body_len <= 0 || (size_t)body_len >= sizeof(body)) {
        Serial.println("[CalDAV] REPORT body too large");
        return -1;
    }

    // Response can be large; allocate on heap to avoid stack overflow.
    const size_t RESP_LEN = 8192;
    char* resp = (char*)malloc(RESP_LEN);
    if (!resp) {
        Serial.println("[CalDAV] OOM allocating response buffer");
        return -1;
    }
    resp[0] = '\0';

    int code = caldav_request("REPORT",
                               CALDAV_CALENDAR_URL,
                               "application/xml; charset=utf-8",
                               body, (size_t)body_len,
                               resp, RESP_LEN);

    int count = 0;
    if (code == 207 || code == 200) {
        count = ical_parse_events(resp, strlen(resp),
                                  CALDAV_CALENDAR_URL,
                                  events, max_events);
        Serial.printf("[CalDAV] Parsed %d event(s)\n", count);
    } else {
        Serial.printf("[CalDAV] list_events failed: HTTP %d\n", code);
        count = -1;
    }

    free(resp);
    return count;
}

// ---------------------------------------------------------------------------
// Create event — PUT
// ---------------------------------------------------------------------------

// Generate a simple UID from a timestamp + random suffix.
static void generate_uid(char* buf, size_t len) {
    uint32_t rnd = esp_random();
    snprintf(buf, len, "esp32-%lu-%08lx@caldav",
             (unsigned long)time(nullptr),
             (unsigned long)rnd);
}

bool caldav_create_event(const char* summary, time_t start_utc, time_t end_utc) {
    char uid[CALDAV_UID_LEN];
    generate_uid(uid, sizeof(uid));

    char ical_body[512];
    size_t ical_len = ical_build_event(ical_body, sizeof(ical_body),
                                       uid, summary,
                                       start_utc, end_utc);
    if (ical_len == 0) {
        Serial.println("[CalDAV] ical_build_event overflow");
        return false;
    }

    // URL: calendar base + uid + ".ics"
    char event_url[CALDAV_URL_LEN];
    snprintf(event_url, sizeof(event_url), "%s%s.ics", CALDAV_CALENDAR_URL, uid);

    int code = caldav_request("PUT",
                               event_url,
                               "text/calendar; charset=utf-8",
                               ical_body, ical_len,
                               nullptr, 0);

    if (code == 201 || code == 204) {
        Serial.printf("[CalDAV] Event created: %s\n", uid);
        return true;
    }

    Serial.printf("[CalDAV] create_event failed: HTTP %d\n", code);
    return false;
}

// ---------------------------------------------------------------------------
// Delete event — DELETE
// ---------------------------------------------------------------------------

bool caldav_delete_event(const char* event_url) {
    int code = caldav_request("DELETE", event_url,
                               nullptr, nullptr, 0,
                               nullptr, 0);

    if (code == 204 || code == 200) {
        Serial.printf("[CalDAV] Deleted: %s\n", event_url);
        return true;
    }

    Serial.printf("[CalDAV] delete_event failed: HTTP %d\n", code);
    return false;
}

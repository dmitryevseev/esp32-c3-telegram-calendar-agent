#include "ical_parser.h"
#include "config.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

// ---------------------------------------------------------------------------
// Field extraction
// ---------------------------------------------------------------------------

// Find the value of an iCal property within a VEVENT block.
// Handles both bare and parameterised forms:
//   SUMMARY:Dentist
//   DTSTART;TZID=Europe/Lisbon:20260325T160000
//
// property  — property name without trailing colon or semicolon (e.g. "SUMMARY")
// Returns true and writes a null-terminated string to out on success.
static bool extract_field(const char* block, size_t block_len,
                           const char* property,
                           char* out, size_t out_len) {
    size_t prop_len = strlen(property);
    const char* p = block;
    const char* end = block + block_len;

    while (p < end) {
        const char* match = (const char*)memmem(p, end - p, property, prop_len);
        if (!match) break;

        // Must be at the start of a line.
        if (match != block && *(match - 1) != '\n') {
            p = match + 1;
            continue;
        }

        // Next char must be ':' or ';' (parameter or value separator).
        const char after = *(match + prop_len);
        if (after != ':' && after != ';') {
            p = match + 1;
            continue;
        }

        // Find the colon that separates any params from the value.
        const char* colon = (const char*)memchr(match + prop_len, ':', end - match - prop_len);
        if (!colon || colon >= end) break;

        const char* val = colon + 1;

        // Value ends at CR or LF.
        const char* val_end = val;
        while (val_end < end && *val_end != '\r' && *val_end != '\n') val_end++;

        size_t copy_len = (size_t)(val_end - val);
        if (copy_len >= out_len) copy_len = out_len - 1;
        memcpy(out, val, copy_len);
        out[copy_len] = '\0';
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Datetime parsing
// ---------------------------------------------------------------------------

// Compute days since Unix epoch for a given year (handles leap years).
static long days_since_epoch(int year, int month, int day) {
    // Cumulative days at start of each month (non-leap).
    static const int mdays[12] = { 0,31,59,90,120,151,181,212,243,273,304,334 };

    long y = year - 1970;
    long leap_days = (year - 1969) / 4 - (year - 1901) / 100 + (year - 1601) / 400;

    // Leap year correction for the current year (if month > Feb).
    bool is_leap = (year % 4 == 0) && (year % 100 != 0 || year % 400 == 0);
    int extra = (is_leap && month > 2) ? 1 : 0;

    return y * 365 + leap_days + mdays[month - 1] + extra + (day - 1);
}

// Convert a broken-down time (treated as UTC) to time_t without using mktime.
static time_t to_utc(int year, int month, int day,
                     int hour, int min, int sec) {
    long days = days_since_epoch(year, month, day);
    return (time_t)(days * 86400L + hour * 3600L + min * 60L + sec);
}

// Parse an iCal datetime value string to time_t UTC.
// Handles:
//   "20260325T150000Z"    — already UTC
//   "20260325T150000"     — local time; subtract TIMEZONE_OFFSET_SEC to get UTC
//   "20260325"            — all-day; midnight local
static time_t parse_datetime(const char* s) {
    if (!s || strlen(s) < 8) return 0;

    int year  = (s[0]-'0')*1000 + (s[1]-'0')*100 + (s[2]-'0')*10 + (s[3]-'0');
    int month = (s[4]-'0')*10  + (s[5]-'0');
    int day   = (s[6]-'0')*10  + (s[7]-'0');
    int hour = 0, min = 0, sec = 0;
    bool is_utc = false;

    if (strlen(s) >= 15 && s[8] == 'T') {
        hour   = (s[9]-'0')*10  + (s[10]-'0');
        min    = (s[11]-'0')*10 + (s[12]-'0');
        sec    = (s[13]-'0')*10 + (s[14]-'0');
        is_utc = (s[15] == 'Z');
    }

    time_t t = to_utc(year, month, day, hour, min, sec);

    if (!is_utc) {
        // Value is in local time — shift to UTC.
        t -= (time_t)TIMEZONE_OFFSET_SEC;
    }

    return t;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

int ical_parse_events(const char* response, size_t response_len,
                      const char* calendar_url,
                      CalEvent* events, int max_events) {
    int count = 0;
    const char* p = response;
    const char* end = response + response_len;

    while (count < max_events) {
        // Find next VEVENT block.
        const char* vevent_start = (const char*)memmem(p, end - p,
                                                       "BEGIN:VEVENT", 12);
        if (!vevent_start) break;

        const char* vevent_end = (const char*)memmem(vevent_start, end - vevent_start,
                                                     "END:VEVENT", 10);
        if (!vevent_end) break;
        vevent_end += 10;  // advance past "END:VEVENT"

        size_t block_len = (size_t)(vevent_end - vevent_start);
        CalEvent& ev = events[count];
        memset(&ev, 0, sizeof(ev));

        // SUMMARY
        if (!extract_field(vevent_start, block_len, "SUMMARY",
                            ev.summary, CALDAV_SUMMARY_LEN)) {
            strncpy(ev.summary, "(no title)", CALDAV_SUMMARY_LEN - 1);
        }

        // UID
        char uid[CALDAV_UID_LEN] = {};
        if (extract_field(vevent_start, block_len, "UID", uid, sizeof(uid))) {
            strncpy(ev.uid, uid, CALDAV_UID_LEN - 1);
        }

        // DTSTART
        char dtstart[32] = {};
        if (extract_field(vevent_start, block_len, "DTSTART", dtstart, sizeof(dtstart))) {
            ev.start_utc = parse_datetime(dtstart);
        }

        // DTEND (fall back to start + 1 hour if missing)
        char dtend[32] = {};
        if (extract_field(vevent_start, block_len, "DTEND", dtend, sizeof(dtend))) {
            ev.end_utc = parse_datetime(dtend);
        }
        if (ev.end_utc == 0 || ev.end_utc <= ev.start_utc) {
            ev.end_utc = ev.start_utc + 3600;
        }

        // Event URL: calendar_url + uid + ".ics"
        // Works for bot-created events (filename == uid.ics).
        // For externally-created events this is a best-effort guess.
        if (uid[0] != '\0') {
            snprintf(ev.event_url, CALDAV_URL_LEN, "%s%s.ics", calendar_url, uid);
        }

        count++;
        p = vevent_end;
    }

    return count;
}

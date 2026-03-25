#pragma once
#include <stddef.h>
#include <time.h>

// Maximum field lengths — keep small to fit in ESP32 RAM budget.
static constexpr int CALDAV_MAX_EVENTS  = 10;
static constexpr int CALDAV_SUMMARY_LEN = 64;
static constexpr int CALDAV_UID_LEN     = 80;
static constexpr int CALDAV_URL_LEN     = 192;

struct CalEvent {
    char    summary[CALDAV_SUMMARY_LEN];
    char    uid[CALDAV_UID_LEN];
    char    event_url[CALDAV_URL_LEN];  // full HTTPS URL for DELETE
    time_t  start_utc;
    time_t  end_utc;
};

// Parse CalEvent structs from a CalDAV REPORT XML response body.
//
// response        — raw response string (may contain XML wrapping iCal data)
// response_len    — length of response string
// calendar_url    — base calendar URL used to construct event_url as
//                   calendar_url + uid + ".ics"
// events          — caller-provided array of at least max_events elements
// max_events      — maximum events to return
//
// Returns number of events parsed (0..max_events).
int ical_parse_events(const char* response, size_t response_len,
                      const char* calendar_url,
                      CalEvent* events, int max_events);

#pragma once
#include <time.h>
#include "ical_parser.h"

// ---------------------------------------------------------------------------
// CalDAV client
// ---------------------------------------------------------------------------
// All network I/O is done sequentially (never overlap with Telegram TLS) to
// keep peak RAM usage within the ESP32-C3 budget.
//
// Call caldav_begin() once after WiFi is up and NTP has synced.
// Subsequent calls to caldav_list/create/delete_event() open a fresh
// WiFiClientSecure + HTTPClient, make the request, then close it.

// Initialise the CalDAV client:
//   - sync system clock via NTP (blocks up to ~5 s)
//   - build the Authorization: Basic header from config.h credentials
void caldav_begin();

// Returns true if the clock has been NTP-synced (time() > some epoch threshold).
bool caldav_time_synced();

// Fetch VEVENT records that overlap [start_utc, end_utc].
// Fills events[] and returns the count (0..max_events on success, -1 on error).
int caldav_list_events(time_t start_utc, time_t end_utc,
                       CalEvent* events, int max_events);

// Create a new event on the server.
// uid is auto-generated; summary must be non-empty.
// Returns true on success (HTTP 201 Created).
bool caldav_create_event(const char* summary, time_t start_utc, time_t end_utc);

// Delete the event at event_url (full HTTPS URL).
// Returns true on success (HTTP 204 No Content).
bool caldav_delete_event(const char* event_url);

#pragma once
#include <time.h>

// Build a minimal iCal VCALENDAR/VEVENT text suitable for a CalDAV PUT request.
//
// uid     — unique ID string (ASCII, no spaces); caller owns it
// summary — event title (UTF-8 allowed)
// start   — event start (UTC)
// end     — event end   (UTC)
// buf     — output buffer
// len     — output buffer length (512 bytes is sufficient for typical events)
//
// Returns the number of bytes written (not including null terminator),
// or 0 on error (buffer too small).
size_t ical_build_event(char* buf, size_t len,
                        const char* uid,
                        const char* summary,
                        time_t start_utc,
                        time_t end_utc);

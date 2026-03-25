#include "ical_builder.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

// Format a time_t value as an iCal UTC timestamp: "YYYYMMDDTHHMMSSz"
static void fmt_utc(char* buf, size_t len, time_t t) {
    struct tm* gm = gmtime(&t);
    strftime(buf, len, "%Y%m%dT%H%M%SZ", gm);
}

size_t ical_build_event(char* buf, size_t len,
                        const char* uid,
                        const char* summary,
                        time_t start_utc,
                        time_t end_utc) {
    char start_str[18], end_str[18], stamp_str[18];
    fmt_utc(start_str, sizeof(start_str), start_utc);
    fmt_utc(end_str,   sizeof(end_str),   end_utc);
    fmt_utc(stamp_str, sizeof(stamp_str), time(nullptr));  // DTSTAMP = now

    int written = snprintf(buf, len,
        "BEGIN:VCALENDAR\r\n"
        "VERSION:2.0\r\n"
        "PRODID:-//ESP32CalBot//EN\r\n"
        "BEGIN:VEVENT\r\n"
        "UID:%s\r\n"
        "DTSTAMP:%s\r\n"
        "DTSTART:%s\r\n"
        "DTEND:%s\r\n"
        "SUMMARY:%s\r\n"
        "END:VEVENT\r\n"
        "END:VCALENDAR\r\n",
        uid, stamp_str, start_str, end_str, summary);

    if (written < 0 || (size_t)written >= len) return 0;
    return (size_t)written;
}

#include "telegram_bot.h"
#include "config.h"
#include "diagnostics.h"
#include "caldav_client.h"
#include "ical_parser.h"
#include "llm_client.h"
#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

static WiFiClientSecure s_tls_client;
static UniversalTelegramBot* s_bot = nullptr;

static uint32_t s_last_poll_ms = 0;

// Event cache — populated by /today and /tomorrow so /delete <N> can reference them.
static CalEvent s_event_cache[CALDAV_MAX_EVENTS];
static int      s_event_cache_count = 0;

// ---------------------------------------------------------------------------
// Access control helpers
// ---------------------------------------------------------------------------

static bool is_user_allowed(const String& user_id) {
    String allowed(TELEGRAM_ALLOWED_USERS);
    int start = 0;
    while (start < (int)allowed.length()) {
        int end = allowed.indexOf(',', start);
        if (end == -1) end = allowed.length();
        String entry = allowed.substring(start, end);
        entry.trim();
        if (entry == user_id) return true;
        start = end + 1;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Time / date helpers
// ---------------------------------------------------------------------------

// Format time_t as a human-readable local date+time string.
static void fmt_local_datetime(char* buf, size_t len, time_t t) {
    time_t local = t + (time_t)TIMEZONE_OFFSET_SEC;
    struct tm* tm = gmtime(&local);
    strftime(buf, len, "%d %b %Y %H:%M", tm);
}

// Format time_t as local time only (HH:MM).
static void fmt_local_time(char* buf, size_t len, time_t t) {
    time_t local = t + (time_t)TIMEZONE_OFFSET_SEC;
    struct tm* tm = gmtime(&local);
    strftime(buf, len, "%H:%M", tm);
}

// Return midnight (local) for day offset relative to today.
// offset=0 → today, offset=1 → tomorrow.
static time_t day_start_utc(int offset) {
    time_t now = time(nullptr);
    time_t local_now = now + (time_t)TIMEZONE_OFFSET_SEC;
    struct tm* t = gmtime(&local_now);
    // Zero out the time portion.
    t->tm_hour = 0;
    t->tm_min  = 0;
    t->tm_sec  = 0;
    // mktime interprets the struct as local, but we stored UTC-shifted values,
    // so we convert via a manual re-calculation using timegm-equivalent.
    // Instead, use to_utc from our own arithmetic (replicated inline here).
    // Simpler: advance by 86400 * offset from today's midnight UTC.
    time_t today_midnight_local = (time_t)((local_now / 86400L) * 86400L);
    time_t today_midnight_utc   = today_midnight_local - (time_t)TIMEZONE_OFFSET_SEC;
    return today_midnight_utc + (time_t)(offset * 86400L);
}

// Parse "HH:MM" into hours + minutes.  Returns false on bad input.
static bool parse_hhmm(const char* s, int& hour, int& min) {
    if (!s || strlen(s) < 4) return false;
    // Accept "HH:MM" or "HHMM".
    if (strlen(s) == 5 && s[2] == ':') {
        hour = (s[0]-'0')*10 + (s[1]-'0');
        min  = (s[3]-'0')*10 + (s[4]-'0');
    } else if (strlen(s) == 4) {
        hour = (s[0]-'0')*10 + (s[1]-'0');
        min  = (s[2]-'0')*10 + (s[3]-'0');
    } else {
        return false;
    }
    return (hour >= 0 && hour <= 23 && min >= 0 && min <= 59);
}

// Parse a date token:
//   "today"     → day_start_utc(0)
//   "tomorrow"  → day_start_utc(1)
//   "DD.MM"     → midnight UTC of next occurrence of that date
//   "DD/MM"     → same
//   "YYYY-MM-DD"→ midnight UTC of that exact date
// Returns day-midnight UTC, or 0 on error.
static time_t parse_date_token(const char* s) {
    if (!s) return 0;

    String tok(s);
    tok.toLowerCase();

    if (tok == "today")    return day_start_utc(0);
    if (tok == "tomorrow") return day_start_utc(1);

    int day = 0, month = 0, year = 0;

    // Try "DD.MM" or "DD/MM"
    if (strlen(s) == 5 && (s[2] == '.' || s[2] == '/')) {
        day   = (s[0]-'0')*10 + (s[1]-'0');
        month = (s[3]-'0')*10 + (s[4]-'0');
        // Determine year: use current year; if the date is in the past, use next year.
        time_t now = time(nullptr);
        time_t local_now = now + (time_t)TIMEZONE_OFFSET_SEC;
        struct tm* t = gmtime(&local_now);
        year = t->tm_year + 1900;

        // Build a candidate time_t for this year.
        // Use a simple month-day → day-of-year calculation (approximate).
        // We re-use day_start_utc logic: offset from Jan 1 midnight.
        // Build via epoch arithmetic consistent with ical_parser.cpp.
        static const int mdays[12] = { 0,31,59,90,120,151,181,212,243,273,304,334 };
        bool is_leap = (year % 4 == 0) && (year % 100 != 0 || year % 400 == 0);
        int extra = (is_leap && month > 2) ? 1 : 0;
        long y = year - 1970;
        long leap_days = (year - 1969) / 4 - (year - 1901) / 100 + (year - 1601) / 400;
        long day_epoch = y * 365 + leap_days + mdays[month - 1] + extra + (day - 1);
        time_t candidate_utc = (time_t)(day_epoch * 86400L) - (time_t)TIMEZONE_OFFSET_SEC;

        // If candidate midnight (local) is in the past, advance to next year.
        if (candidate_utc < now) {
            year++;
            bool is_leap2 = (year % 4 == 0) && (year % 100 != 0 || year % 400 == 0);
            int extra2 = (is_leap2 && month > 2) ? 1 : 0;
            long y2 = year - 1970;
            long ld2 = (year - 1969) / 4 - (year - 1901) / 100 + (year - 1601) / 400;
            long de2 = y2 * 365 + ld2 + mdays[month - 1] + extra2 + (day - 1);
            candidate_utc = (time_t)(de2 * 86400L) - (time_t)TIMEZONE_OFFSET_SEC;
        }
        return (day >= 1 && day <= 31 && month >= 1 && month <= 12) ? candidate_utc : 0;
    }

    // Try "YYYY-MM-DD"
    if (strlen(s) == 10 && s[4] == '-' && s[7] == '-') {
        year  = (s[0]-'0')*1000 + (s[1]-'0')*100 + (s[2]-'0')*10 + (s[3]-'0');
        month = (s[5]-'0')*10 + (s[6]-'0');
        day   = (s[8]-'0')*10 + (s[9]-'0');
        static const int mdays[12] = { 0,31,59,90,120,151,181,212,243,273,304,334 };
        bool is_leap = (year % 4 == 0) && (year % 100 != 0 || year % 400 == 0);
        int extra = (is_leap && month > 2) ? 1 : 0;
        long y = year - 1970;
        long ld = (year - 1969) / 4 - (year - 1901) / 100 + (year - 1601) / 400;
        long de = y * 365 + ld + mdays[month - 1] + extra + (day - 1);
        return (time_t)(de * 86400L) - (time_t)TIMEZONE_OFFSET_SEC;
    }

    return 0;
}

// ---------------------------------------------------------------------------
// Event listing helper
// ---------------------------------------------------------------------------

// Format the cached event list as a Telegram message.
static String format_event_list(int count, const char* header) {
    String msg(header);
    msg += "\n";
    if (count <= 0) {
        msg += "(no events)";
        return msg;
    }
    for (int i = 0; i < count; i++) {
        char t_start[20], t_end[20];
        fmt_local_time(t_start, sizeof(t_start), s_event_cache[i].start_utc);
        fmt_local_time(t_end,   sizeof(t_end),   s_event_cache[i].end_utc);
        char line[160];
        snprintf(line, sizeof(line), "%d. %s  %s–%s\n",
                 i + 1,
                 s_event_cache[i].summary,
                 t_start, t_end);
        msg += line;
    }
    msg += "\nUse /delete <number> to remove an event.";
    return msg;
}

// ---------------------------------------------------------------------------
// Command handlers
// ---------------------------------------------------------------------------

static void handle_start(const telegramMessage& msg) {
    String reply =
        "Hi " + msg.from_name + "! I'm your family calendar bot.\n\n"
        "Just type naturally — e.g. \"what's on tomorrow?\" or "
        "\"add dentist Friday at 3pm\".\n\n"
        "Slash commands (instant, no AI):\n"
        "/today          — list today's events\n"
        "/tomorrow       — list tomorrow's events\n"
        "/add <date> <HH:MM> <title>\n"
        "                — add an event (date: today/tomorrow/DD.MM/YYYY-MM-DD)\n"
        "/delete <N>     — delete event N from last listing\n"
        "/ping           — check if I'm alive\n"
        "/help           — show this message";
    s_bot->sendMessage(msg.chat_id, reply, "");
}

static void handle_help(const telegramMessage& msg) {
    handle_start(msg);
}

static void handle_ping(const telegramMessage& msg) {
    char buf[80];
    snprintf(buf, sizeof(buf),
             "Pong! Free heap: %lu bytes", (unsigned long)esp_get_free_heap_size());
    s_bot->sendMessage(msg.chat_id, String(buf), "");
}

static void handle_today(const telegramMessage& msg) {
    if (!caldav_time_synced()) {
        s_bot->sendMessage(msg.chat_id, "Clock not synced yet — try again in a moment.", "");
        return;
    }
    time_t start = day_start_utc(0);
    time_t end   = start + 86400L;

    s_bot->sendMessage(msg.chat_id, "Fetching today's events...", "");
    s_event_cache_count = caldav_list_events(start, end, s_event_cache, CALDAV_MAX_EVENTS);

    char header[48];
    struct tm* t = gmtime(&start);  // start is already local-midnight in UTC
    // Format as "Today — DD Mon YYYY"
    time_t local_start = start + (time_t)TIMEZONE_OFFSET_SEC;
    struct tm* lt = gmtime(&local_start);
    strftime(header, sizeof(header), "Today — %d %b %Y", lt);

    if (s_event_cache_count < 0) {
        s_bot->sendMessage(msg.chat_id, "Failed to fetch events. Check CalDAV config.", "");
    } else {
        s_bot->sendMessage(msg.chat_id, format_event_list(s_event_cache_count, header), "");
    }
    (void)t;
}

static void handle_tomorrow(const telegramMessage& msg) {
    if (!caldav_time_synced()) {
        s_bot->sendMessage(msg.chat_id, "Clock not synced yet — try again in a moment.", "");
        return;
    }
    time_t start = day_start_utc(1);
    time_t end   = start + 86400L;

    s_bot->sendMessage(msg.chat_id, "Fetching tomorrow's events...", "");
    s_event_cache_count = caldav_list_events(start, end, s_event_cache, CALDAV_MAX_EVENTS);

    char header[52];
    time_t local_start = start + (time_t)TIMEZONE_OFFSET_SEC;
    struct tm* lt = gmtime(&local_start);
    strftime(header, sizeof(header), "Tomorrow — %d %b %Y", lt);

    if (s_event_cache_count < 0) {
        s_bot->sendMessage(msg.chat_id, "Failed to fetch events. Check CalDAV config.", "");
    } else {
        s_bot->sendMessage(msg.chat_id, format_event_list(s_event_cache_count, header), "");
    }
}

// /add <date> <HH:MM> <title with spaces>
// Example: /add tomorrow 15:00 Dentist
// Example: /add 28.03 09:30 School play
static void handle_add(const telegramMessage& msg) {
    // Strip the "/add " prefix.
    String text = msg.text;
    int space1 = text.indexOf(' ');
    if (space1 < 0) {
        s_bot->sendMessage(msg.chat_id,
            "Usage: /add <date> <HH:MM> <title>\n"
            "Date: today, tomorrow, DD.MM, or YYYY-MM-DD", "");
        return;
    }
    String rest = text.substring(space1 + 1);
    rest.trim();

    // Split into three tokens: date, time, title
    int sp2 = rest.indexOf(' ');
    if (sp2 < 0) {
        s_bot->sendMessage(msg.chat_id, "Missing time or title. Usage: /add <date> <HH:MM> <title>", "");
        return;
    }
    String date_tok = rest.substring(0, sp2);
    String rest2 = rest.substring(sp2 + 1);
    rest2.trim();

    int sp3 = rest2.indexOf(' ');
    if (sp3 < 0) {
        s_bot->sendMessage(msg.chat_id, "Missing title. Usage: /add <date> <HH:MM> <title>", "");
        return;
    }
    String time_tok  = rest2.substring(0, sp3);
    String title     = rest2.substring(sp3 + 1);
    title.trim();

    if (title.length() == 0) {
        s_bot->sendMessage(msg.chat_id, "Event title cannot be empty.", "");
        return;
    }

    // Parse date.
    time_t day_midnight = parse_date_token(date_tok.c_str());
    if (day_midnight == 0) {
        s_bot->sendMessage(msg.chat_id,
            "Could not parse date. Use: today, tomorrow, DD.MM, or YYYY-MM-DD", "");
        return;
    }

    // Parse time.
    int hour = 0, min = 0;
    if (!parse_hhmm(time_tok.c_str(), hour, min)) {
        s_bot->sendMessage(msg.chat_id, "Could not parse time. Use HH:MM (e.g. 15:00).", "");
        return;
    }

    time_t start_utc = day_midnight + (time_t)(hour * 3600 + min * 60);
    time_t end_utc   = start_utc + 3600;  // 1-hour default duration

    char confirm[160];
    char dt_str[24];
    fmt_local_datetime(dt_str, sizeof(dt_str), start_utc);
    snprintf(confirm, sizeof(confirm), "Adding: \"%s\" at %s…", title.c_str(), dt_str);
    s_bot->sendMessage(msg.chat_id, String(confirm), "");

    if (caldav_create_event(title.c_str(), start_utc, end_utc)) {
        char ok[160];
        snprintf(ok, sizeof(ok), "Event added: \"%s\" at %s", title.c_str(), dt_str);
        s_bot->sendMessage(msg.chat_id, String(ok), "");
    } else {
        s_bot->sendMessage(msg.chat_id, "Failed to create event. Check CalDAV credentials.", "");
    }
}

// /delete <N>  — deletes event N from the last listing (1-based index)
static void handle_delete(const telegramMessage& msg) {
    if (s_event_cache_count <= 0) {
        s_bot->sendMessage(msg.chat_id,
            "No events cached. Use /today or /tomorrow first.", "");
        return;
    }

    String text = msg.text;
    int space = text.indexOf(' ');
    if (space < 0) {
        s_bot->sendMessage(msg.chat_id, "Usage: /delete <number>  (e.g. /delete 2)", "");
        return;
    }
    int n = text.substring(space + 1).toInt();
    if (n < 1 || n > s_event_cache_count) {
        char buf[80];
        snprintf(buf, sizeof(buf),
                 "Invalid number. Choose 1–%d from the last listing.", s_event_cache_count);
        s_bot->sendMessage(msg.chat_id, String(buf), "");
        return;
    }

    const CalEvent& ev = s_event_cache[n - 1];

    if (ev.event_url[0] == '\0') {
        s_bot->sendMessage(msg.chat_id,
            "Cannot delete: event URL is unknown (externally created event).", "");
        return;
    }

    char confirm[160];
    snprintf(confirm, sizeof(confirm), "Deleting \"%s\"…", ev.summary);
    s_bot->sendMessage(msg.chat_id, String(confirm), "");

    if (caldav_delete_event(ev.event_url)) {
        char ok[160];
        snprintf(ok, sizeof(ok), "Deleted: \"%s\"", ev.summary);
        s_bot->sendMessage(msg.chat_id, String(ok), "");
        // Remove the entry from the cache.
        for (int i = n - 1; i < s_event_cache_count - 1; i++) {
            s_event_cache[i] = s_event_cache[i + 1];
        }
        s_event_cache_count--;
    } else {
        s_bot->sendMessage(msg.chat_id, "Failed to delete event.", "");
    }
}

// ---------------------------------------------------------------------------
// LLM-based natural language handler (plain-text messages)
// ---------------------------------------------------------------------------

static void handle_unknown(const telegramMessage& msg);  // forward declaration

static void handle_llm_message(const telegramMessage& msg) {
#if LLM_ENABLED
    if (!caldav_time_synced()) {
        s_bot->sendMessage(msg.chat_id,
            "Clock not synced yet — try again in a moment.", "");
        return;
    }

    // Build today string for the system prompt.
    char today_str[12];
    time_t now = time(nullptr);
    time_t local_now = now + (time_t)TIMEZONE_OFFSET_SEC;
    struct tm* lt = gmtime(&local_now);
    strftime(today_str, sizeof(today_str), "%Y-%m-%d", lt);

    // Build compact event context from cache (used as hint for delete).
    char event_ctx[300] = "";
    if (s_event_cache_count > 0) {
        int pos = 0;
        for (int i = 0; i < s_event_cache_count && pos < (int)sizeof(event_ctx) - 40; i++) {
            char t_start[6], t_end[6];
            fmt_local_time(t_start, sizeof(t_start), s_event_cache[i].start_utc);
            fmt_local_time(t_end,   sizeof(t_end),   s_event_cache[i].end_utc);
            pos += snprintf(event_ctx + pos, sizeof(event_ctx) - pos,
                            "%d. %s %s-%s\n",
                            i + 1, s_event_cache[i].summary, t_start, t_end);
        }
    }

    // Let the user know we're working on it.
    s_bot->sendMessage(msg.chat_id, "Thinking...", "");

    LlmToolCall tc = llm_classify(msg.text.c_str(),
                                   event_ctx[0] ? event_ctx : nullptr,
                                   today_str);

    if (!tc.valid) {
        s_bot->sendMessage(msg.chat_id, String(tc.param_message), "");
        return;
    }

    // --- Dispatch on tool name -------------------------------------------

    if (strcmp(tc.tool_name, "list_events") == 0) {
        time_t start = parse_date_token(tc.param_date_start);
        if (start == 0) {
            s_bot->sendMessage(msg.chat_id, "Could not parse date from AI response.", "");
            return;
        }
        time_t end_t = tc.param_date_end[0]
                       ? parse_date_token(tc.param_date_end) + 86400L
                       : start + 86400L;

        int count = caldav_list_events(start, end_t, s_event_cache, CALDAV_MAX_EVENTS);
        s_event_cache_count = (count >= 0) ? count : 0;
        if (count < 0) {
            s_bot->sendMessage(msg.chat_id, "Failed to fetch events.", "");
            return;
        }
        char header[64];
        time_t local_start = start + (time_t)TIMEZONE_OFFSET_SEC;
        struct tm* lts = gmtime(&local_start);
        if (end_t - start <= 86400L) {
            strftime(header, sizeof(header), "Events on %d %b %Y", lts);
        } else {
            char end_fmt[32];
            time_t local_end = (end_t - 86400L) + (time_t)TIMEZONE_OFFSET_SEC;
            struct tm* lte = gmtime(&local_end);
            char start_fmt[20];
            strftime(start_fmt, sizeof(start_fmt), "%d %b", lts);
            strftime(end_fmt,   sizeof(end_fmt),   "%d %b %Y", lte);
            snprintf(header, sizeof(header), "Events %s – %s", start_fmt, end_fmt);
        }
        s_bot->sendMessage(msg.chat_id, format_event_list(count, header), "");
    }

    else if (strcmp(tc.tool_name, "create_event") == 0) {
        time_t day_midnight = parse_date_token(tc.param_date_start);
        if (day_midnight == 0) {
            s_bot->sendMessage(msg.chat_id, "Could not parse date.", "");
            return;
        }
        int hour = 0, min_v = 0;
        if (!parse_hhmm(tc.param_time, hour, min_v)) {
            s_bot->sendMessage(msg.chat_id, "Could not parse time.", "");
            return;
        }
        if (tc.param_title[0] == '\0') {
            s_bot->sendMessage(msg.chat_id, "Event title is missing.", "");
            return;
        }
        time_t start_utc = day_midnight + (time_t)(hour * 3600 + min_v * 60);
        int dur = (tc.param_duration > 0) ? tc.param_duration : 60;
        time_t end_utc = start_utc + (time_t)(dur * 60);

        char dt_str[24];
        fmt_local_datetime(dt_str, sizeof(dt_str), start_utc);

        if (caldav_create_event(tc.param_title, start_utc, end_utc)) {
            char ok[160];
            snprintf(ok, sizeof(ok), "Event added: \"%s\" at %s", tc.param_title, dt_str);
            s_bot->sendMessage(msg.chat_id, String(ok), "");
        } else {
            s_bot->sendMessage(msg.chat_id, "Failed to create event. Check CalDAV config.", "");
        }
    }

    else if (strcmp(tc.tool_name, "delete_event") == 0) {
        if (s_event_cache_count <= 0) {
            s_bot->sendMessage(msg.chat_id,
                "No events cached. Ask me to list events first.", "");
            return;
        }
        int n = tc.param_index;
        if (n < 1 || n > s_event_cache_count) {
            char buf[80];
            snprintf(buf, sizeof(buf),
                     "Invalid number. Choose 1–%d from last listing.", s_event_cache_count);
            s_bot->sendMessage(msg.chat_id, String(buf), "");
            return;
        }
        const CalEvent& ev = s_event_cache[n - 1];
        if (ev.event_url[0] == '\0') {
            s_bot->sendMessage(msg.chat_id,
                "Cannot delete: event URL unknown.", "");
            return;
        }
        if (caldav_delete_event(ev.event_url)) {
            char ok[160];
            snprintf(ok, sizeof(ok), "Deleted: \"%s\"", ev.summary);
            s_bot->sendMessage(msg.chat_id, String(ok), "");
            for (int i = n - 1; i < s_event_cache_count - 1; i++) {
                s_event_cache[i] = s_event_cache[i + 1];
            }
            s_event_cache_count--;
        } else {
            s_bot->sendMessage(msg.chat_id, "Failed to delete event.", "");
        }
    }

    else if (strcmp(tc.tool_name, "reply") == 0) {
        s_bot->sendMessage(msg.chat_id, String(tc.param_message), "");
    }

    else if (strcmp(tc.tool_name, "ping") == 0) {
        char buf[80];
        snprintf(buf, sizeof(buf),
                 "Pong! Free heap: %lu bytes", (unsigned long)esp_get_free_heap_size());
        s_bot->sendMessage(msg.chat_id, String(buf), "");
    }

    else {
        s_bot->sendMessage(msg.chat_id,
            "AI returned an unknown action. Use /help for slash commands.", "");
    }
#else
    handle_unknown(msg);
#endif  // LLM_ENABLED
}

static void handle_unknown(const telegramMessage& msg) {
    s_bot->sendMessage(msg.chat_id,
        "Unknown command. Send /help for a list of commands.", "");
}

// ---------------------------------------------------------------------------
// Message dispatcher
// ---------------------------------------------------------------------------

static void dispatch(const telegramMessage& msg) {
    if (msg.type != "message") return;

    Serial.printf("[Bot] Message from %s (%s): %s\n",
                  msg.from_name.c_str(),
                  msg.from_id.c_str(),
                  msg.text.c_str());

    if (!is_user_allowed(msg.from_id)) {
        Serial.printf("[Bot] Rejected: user %s not in allowlist\n",
                      msg.from_id.c_str());
        s_bot->sendMessage(msg.chat_id,
            "Sorry, you are not authorised to use this bot.", "");
        return;
    }

    if      (msg.text == "/start"    || msg.text.startsWith("/start "))    handle_start(msg);
    else if (msg.text == "/help"     || msg.text.startsWith("/help "))     handle_help(msg);
    else if (msg.text == "/ping"     || msg.text.startsWith("/ping "))     handle_ping(msg);
    else if (msg.text == "/today"    || msg.text.startsWith("/today "))    handle_today(msg);
    else if (msg.text == "/tomorrow" || msg.text.startsWith("/tomorrow ")) handle_tomorrow(msg);
    else if (msg.text.startsWith("/add "))                                 handle_add(msg);
    else if (msg.text.startsWith("/delete "))                              handle_delete(msg);
    else if (msg.text.startsWith("/"))                                     handle_unknown(msg);
    else                                                                   handle_llm_message(msg);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void bot_begin() {
    s_tls_client.setInsecure();
    s_bot = new UniversalTelegramBot(TELEGRAM_BOT_TOKEN, s_tls_client);
    // Drain queued messages from before boot.
    s_bot->getUpdates(s_bot->last_message_received + 1);
    Serial.println("[Bot] Initialized. Polling for messages...");
}

void bot_poll() {
    if (millis() - s_last_poll_ms < TELEGRAM_POLL_INTERVAL_MS) return;
    s_last_poll_ms = millis();

    diag_heap_before_poll();
    int num_messages = s_bot->getUpdates(s_bot->last_message_received + 1);
    diag_heap_after_poll();

    for (int i = 0; i < num_messages; i++) {
        dispatch(s_bot->messages[i]);
    }
}

# ESP32-C3 Telegram Calendar Agent

An ESP32-C3 firmware that connects a Telegram bot to your iCloud calendar via CalDAV. Send natural-language messages or slash commands to list, add, and delete events.

## Features

- **Natural language**: "what's on tomorrow?", "add dentist Friday at 3pm" — powered by Claude Haiku
- **Slash commands**: instant responses, no API calls required
- **iCloud CalDAV**: reads and writes to your iCloud calendar
- **Multi-user**: whitelist multiple Telegram user IDs
- **Low footprint**: runs on ESP32-C3 with ~174 KB free heap, sequential TLS to stay within RAM limits

## Hardware

- ESP32-C3-DevKitM-1 (or any ESP32-C3 board)
- USB cable for flashing

## Prerequisites

- [PlatformIO](https://platformio.org/) (CLI or VS Code extension)
- Telegram bot token — create one via [@BotFather](https://t.me/BotFather)
- Your Telegram user ID — get it from [@userinfobot](https://t.me/userinfobot)
- iCloud app-specific password — generate at [appleid.apple.com](https://appleid.apple.com) → Sign-In & Security → App-Specific Passwords
- Claude API key — [console.anthropic.com](https://console.anthropic.com) (optional; set `LLM_ENABLED 0` to skip)

## Setup

### 1. Clone and configure

```bash
git clone <repo-url>
cd esp32-c3-calendar-agent/firmware
cp include/config.h.example include/config.h
```

Edit `include/config.h` and fill in:

| Define | Description |
|--------|-------------|
| `WIFI_SSID` / `WIFI_PASSWORD` | Your WiFi credentials |
| `TELEGRAM_BOT_TOKEN` | Token from @BotFather |
| `TELEGRAM_ALLOWED_USERS` | Comma-separated Telegram user IDs |
| `CALDAV_APPLE_ID` | Your iCloud email address |
| `CALDAV_APP_PASSWORD` | App-specific password (`xxxx-xxxx-xxxx-xxxx`) |
| `CALDAV_CALENDAR_URL` | Full CalDAV URL (see below) |
| `TIMEZONE_OFFSET_SEC` | UTC offset in seconds — e.g. `(1 * 3600)` for UTC+1 |
| `CLAUDE_API_KEY` | Claude API key (if `LLM_ENABLED 1`) |

### 2. Find your CalDAV URL

```bash
# Uses curl + your Apple credentials to discover the URL
bash tools/find_caldav_url.sh
```

Or set it manually — format: `https://pXX-caldav.icloud.com/<DS_NUMBER>/calendars/<UUID>/`

### 3. Build and flash

```bash
pio run -e esp32-c3-devkitm-1 -t upload --upload-port /dev/cu.usbmodemXXXX
```

### 4. Monitor

```bash
pio device monitor --port /dev/cu.usbmodemXXXX --baud 115200
```

## Telegram Commands

| Command | Description |
|---------|-------------|
| `/today` | List today's events |
| `/tomorrow` | List tomorrow's events |
| `/add <date> <HH:MM> <title>` | Add an event |
| `/delete <N>` | Delete event N from the last listing |
| `/ping` | Health check — returns free heap |
| `/help` | Show command reference |

**Date formats for `/add`:** `today`, `tomorrow`, `DD.MM`, `YYYY-MM-DD`

**Natural language** (requires `LLM_ENABLED 1`): type any plain-text message and Claude will figure out the intent.

## Timezone and DST

The firmware uses a fixed `TIMEZONE_OFFSET_SEC` — there is no automatic DST handling. Update `config.h` and reflash when your region changes offset.

Portugal example:
- WET (winter): `(0 * 3600)` — standard time until last Sunday of March
- WEST (summer): `(1 * 3600)` — DST from last Sunday of March to last Sunday of October

## Architecture

```
main.cpp
 ├── wifi_manager     — STA connection, exponential-backoff reconnect, LED status
 ├── telegram_bot     — poll updates, dispatch slash commands, route plain text to LLM
 │    ├── caldav_client  — REPORT / PUT / DELETE over HTTPS to iCloud CalDAV
 │    │    ├── ical_parser   — parse VEVENT blocks from CalDAV XML response
 │    │    └── ical_builder  — generate iCalendar text for PUT requests
 │    └── llm_client   — POST to Claude Messages API, parse tool_use response
 └── diagnostics      — heap monitoring, watchdog reboot on low memory
```

**RAM strategy**: TLS sessions are strictly sequential (Telegram → CalDAV → Claude). The mbedTLS I/O buffers are reduced from 16 KB to 4 KB each. The `loopTask` stack is set to 16 KB (ECC handshake needs ~6 KB alone).

## Configuration Reference

All tuning parameters are in `include/config.h` / `include/config.h.example`:

| Define | Default | Description |
|--------|---------|-------------|
| `LLM_ENABLED` | `1` | Set to `0` for slash-command-only mode |
| `CLAUDE_MODEL` | `claude-haiku-4-5-20251001` | Model for intent classification |
| `CLAUDE_API_TIMEOUT_MS` | `30000` | Claude API request timeout |
| `TELEGRAM_POLL_INTERVAL_MS` | `3000` | How often to check for new messages |
| `HEAP_REBOOT_THRESHOLD` | `20000` | Auto-reboot if free heap drops below this |
| `STATUS_LED_PIN` | `8` | GPIO for LED indicator (`-1` to disable) |

## LED Status

| Pattern | Meaning |
|---------|---------|
| Fast blink (200 ms) | Connecting to WiFi |
| Slow blink (1000 ms) | Connected, polling Telegram |
| Solid on | Processing a message |
| Off | Error / restarting |

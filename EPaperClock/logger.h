#pragma once
// Append-only climate log in internal flash (LittleFS): 8 bytes per sample,
// one sample per 10 minutes = about 8 KB for a week.
#include <LittleFS.h>
#include <stdint.h>

#pragma pack(push, 1)
struct LogRec {
  uint32_t t;    // epoch seconds (UTC)
  int16_t tc;    // temperature, degrees C x 100
  uint16_t rh;   // relative humidity, percent x 100
};
#pragma pack(pop)

#define LOG_PATH "/log.bin"
#define LOG_KEEP_DAYS 8
#define LOG_MAX_KEEP 1300  // 8 days at 10-minute spacing, with slack

static bool logBegin() { return LittleFS.begin(true); }

static size_t logCount() {
  File f = LittleFS.open(LOG_PATH, "r");
  if (!f) return 0;
  size_t n = f.size() / sizeof(LogRec);
  f.close();
  return n;
}

static bool logAppend(const LogRec &r) {
  File f = LittleFS.open(LOG_PATH, "a");
  if (!f) return false;
  size_t w = f.write((const uint8_t *)&r, sizeof r);
  f.close();
  return w == sizeof r;
}

// Reads records with t >= since into out (oldest first); returns how many.
static size_t logLoad(uint32_t since, LogRec *out, size_t maxN) {
  File f = LittleFS.open(LOG_PATH, "r");
  if (!f) return 0;
  size_t n = 0;
  LogRec r;
  while (f.read((uint8_t *)&r, sizeof r) == sizeof r)
    if (r.t >= since && n < maxN) out[n++] = r;
  f.close();
  return n;
}

// Drops everything older than LOG_KEEP_DAYS once the file has grown well past that.
static void logCompact(uint32_t now) {
  if (logCount() < 2 * LOG_MAX_KEEP) return;
  static LogRec buf[LOG_MAX_KEEP];
  size_t n = logLoad(now - LOG_KEEP_DAYS * 86400UL, buf, LOG_MAX_KEEP);
  File f = LittleFS.open(LOG_PATH, "w");
  if (!f) return;
  f.write((const uint8_t *)buf, n * sizeof(LogRec));
  f.close();
}

static void logClear() { LittleFS.remove(LOG_PATH); }

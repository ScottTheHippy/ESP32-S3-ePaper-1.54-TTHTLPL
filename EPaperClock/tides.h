#pragma once
// NOAA CO-OPS tide predictions (free, no key): high/low events for a station, cached in flash
// so the clock keeps showing tides for days without Wi-Fi.
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <WiFiClientSecure.h>
#include <math.h>
#include "timeutil.h"

struct TideEv {
  uint32_t t;  // epoch seconds (UTC)
  float m;     // height in metres above MLLW
  char type;   // 'H' or 'L'
};

#define MAX_TIDES 64
#define TIDE_PATH "/tides.bin"
static TideEv tideEv[MAX_TIDES];
static int tideCount = 0;
static char tideLoadedStation[16] = "";
static SemaphoreHandle_t tideLock = nullptr;

#pragma pack(push, 1)
struct TideFileHdr {
  uint32_t magic;
  char station[16];
  uint16_t count;
};
#pragma pack(pop)
#define TIDE_MAGIC 0x45444954u

static void tidesInit() {
  if (!tideLock) tideLock = xSemaphoreCreateMutex();
  File f = LittleFS.open(TIDE_PATH, "r");
  if (!f) return;
  TideFileHdr h;
  if (f.read((uint8_t *)&h, sizeof h) == sizeof h && h.magic == TIDE_MAGIC && h.count <= MAX_TIDES) {
    xSemaphoreTake(tideLock, portMAX_DELAY);
    tideCount = f.read((uint8_t *)tideEv, h.count * sizeof(TideEv)) / sizeof(TideEv);
    strlcpy(tideLoadedStation, h.station, sizeof tideLoadedStation);
    xSemaphoreGive(tideLock);
  }
  f.close();
}

static void tidesSet(const TideEv *ev, int n, const char *station) {
  xSemaphoreTake(tideLock, portMAX_DELAY);
  tideCount = min(n, MAX_TIDES);
  memcpy(tideEv, ev, tideCount * sizeof(TideEv));
  strlcpy(tideLoadedStation, station, sizeof tideLoadedStation);
  File f = LittleFS.open(TIDE_PATH, "w");
  if (f) {
    TideFileHdr h = {TIDE_MAGIC, {0}, (uint16_t)tideCount};
    strlcpy(h.station, station, sizeof h.station);
    f.write((const uint8_t *)&h, sizeof h);
    f.write((const uint8_t *)tideEv, tideCount * sizeof(TideEv));
    f.close();
  }
  xSemaphoreGive(tideLock);
}

static void tidesClear() {
  xSemaphoreTake(tideLock, portMAX_DELAY);
  tideCount = 0;
  tideLoadedStation[0] = 0;
  xSemaphoreGive(tideLock);
  LittleFS.remove(TIDE_PATH);
}

static int tidesSnapshot(TideEv *dst, int maxN) {
  xSemaphoreTake(tideLock, portMAX_DELAY);
  int n = min(tideCount, maxN);
  memcpy(dst, tideEv, n * sizeof(TideEv));
  xSemaphoreGive(tideLock);
  return n;
}

// Hours of predictions still ahead of `now` (0 if none).
static float tidesCoverageHours(time_t now) {
  xSemaphoreTake(tideLock, portMAX_DELAY);
  float h = tideCount ? ((int64_t)tideEv[tideCount - 1].t - (int64_t)now) / 3600.0f : 0;
  xSemaphoreGive(tideLock);
  return h < 0 ? 0 : h;
}

// Fetches ~9 days of high/low predictions. Returns the number of events stored (0 = failure).
static int tidesFetch(const String &station) {
  time_t now = time(nullptr);
  struct tm a, b;
  time_t t0 = now - 86400, t1 = now + 8 * 86400L;
  gmtime_r(&t0, &a);
  gmtime_r(&t1, &b);
  char url[300];
  snprintf(url, sizeof url,
           "https://api.tidesandcurrents.noaa.gov/api/prod/datagetter?product=predictions&application=esp32-epaper-clock"
           "&begin_date=%04d%02d%02d&end_date=%04d%02d%02d&datum=MLLW&station=%s&time_zone=gmt&units=metric&interval=hilo&format=json",
           a.tm_year + 1900, a.tm_mon + 1, a.tm_mday, b.tm_year + 1900, b.tm_mon + 1, b.tm_mday, station.c_str());

  WiFiClientSecure client;
  client.setInsecure();  // public prediction data, nothing secret is sent
  client.setTimeout(15);
  HTTPClient http;
  http.useHTTP10(true);
  http.setConnectTimeout(10000);
  http.setTimeout(15000);
  if (!http.begin(client, url)) return 0;
  int code = http.GET();
  if (code != 200) {
    Serial.printf("tides: http %d\n", code);
    http.end();
    return 0;
  }
  JsonDocument filter, doc;
  filter["predictions"][0]["t"] = true;
  filter["predictions"][0]["v"] = true;
  filter["predictions"][0]["type"] = true;
  DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    Serial.printf("tides: json error %s\n", err.c_str());
    return 0;
  }
  static TideEv tmp[MAX_TIDES];
  int n = 0;
  for (JsonObject p : doc["predictions"].as<JsonArray>()) {
    if (n >= MAX_TIDES) break;
    time_t t = parseGmt(p["t"] | "");
    const char *ty = p["type"] | "";
    if (!t || (ty[0] != 'H' && ty[0] != 'L')) continue;
    tmp[n++] = {(uint32_t)t, (float)(p["v"].as<float>()), ty[0]};
  }
  if (n == 0) {
    Serial.println("tides: no predictions in response (bad station id?)");
    return 0;
  }
  tidesSet(tmp, n, station.c_str());
  Serial.printf("tides: stored %d events for station %s\n", n, station.c_str());
  return n;
}

// Station display name, e.g. "Seattle, WA".
static bool tidesFetchName(const String &station, String &out) {
  char url[160];
  snprintf(url, sizeof url, "https://api.tidesandcurrents.noaa.gov/mdapi/prod/webapi/stations/%s.json", station.c_str());
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(15);
  HTTPClient http;
  http.useHTTP10(true);
  http.setConnectTimeout(10000);
  http.setTimeout(15000);
  if (!http.begin(client, url)) return false;
  int code = http.GET();
  if (code != 200) { http.end(); return false; }
  JsonDocument filter, doc;
  filter["stations"][0]["name"] = true;
  filter["stations"][0]["state"] = true;
  DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) return false;
  const char *nm = doc["stations"][0]["name"] | "";
  const char *st = doc["stations"][0]["state"] | "";
  if (!nm[0]) return false;
  out = String(nm);
  if (st[0]) out += String(", ") + st;
  return true;
}

// Cosine interpolation between the surrounding high/low events (the standard clock-face tide curve).
static bool tideHeightAt(const TideEv *ev, int n, time_t t, float &h) {
  for (int i = 0; i + 1 < n; i++) {
    if ((time_t)ev[i].t <= t && t < (time_t)ev[i + 1].t) {
      float f = (float)(t - (time_t)ev[i].t) / (float)(ev[i + 1].t - ev[i].t);
      h = (ev[i].m + ev[i + 1].m) * 0.5f + (ev[i].m - ev[i + 1].m) * 0.5f * cosf(M_PI * f);
      return true;
    }
  }
  return false;
}

// Desk clock for the Waveshare ESP32-S3-ePaper-1.54 (200x200 black/white e-paper):
//   page 1  clock, date, temperature + humidity, next tides, moon phase
//   page 2  last 7 days of temperature and humidity
//   page 3  tide curve for the surrounding 24 hours
// Climate samples are logged every 10 minutes to internal flash (the 7-day graph) and, if a
// microSD card is present, to /climate/log.csv as permanent history. Tides come from NOAA
// (free, no key) and are cached so they keep working without Wi-Fi. The moon is computed locally.
// Buttons: BOOT = next page, PWR = previous page, hold BOOT 2 s = Wi-Fi/settings setup.
// Build: FQBN esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=8M,PartitionScheme=huge_app
#include <Adafruit_GFX.h>
#include <Fonts/FreeSans9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <FS.h>
#include <GxEPD2_BW.h>
#include <Preferences.h>
#include <SD_MMC.h>
#include <Wire.h>
#include <sys/time.h>
#include <time.h>
#include "qrcode.h"
#include "logger.h"
#include "moon.h"
#include "tides.h"
#include "timeutil.h"

// ---- hardware (pins from Waveshare's demo repo for this board) ----
#define EPD_DC 10
#define EPD_CS 11
#define EPD_SCK 12
#define EPD_MOSI 13
#define EPD_RST 9
#define EPD_BUSY 8
#define EPD_PWR 6     // active low: powers the panel
#define AUDIO_PWR 42  // active low: powers the RTC / temperature sensor / codec I2C devices
#define VBAT_LATCH 17 // hold high to stay powered when running from a battery
#define BOOT_BTN 0
#define PWR_BTN 18
#define I2C_SDA 47
#define I2C_SCL 48
#define RTC_ADDR 0x51
#define SHTC3_ADDR 0x70
#define VBAT_ADC 4    // battery voltage through a 2:1 divider (ADC1 channel 3)
#define SD_CLK 39
#define SD_CMD 41
#define SD_D0 40

#define LOCAL_TZ "PST8PDT,M3.2.0,M11.1.0"  // default until chosen in the phone setup
#define SETUP_AP_SSID "EPaper-Clock-Setup"
#define TIDE_DEFAULT_STATION "9447130"     // a major station; set your own in the phone setup
#define LOG_INTERVAL_MIN 10
#define ROTATION 0

#define INK 0x0000
#define PAPER 0xFFFF

GxEPD2_BW<GxEPD2_154_D67, GxEPD2_154_D67::HEIGHT> display(GxEPD2_154_D67(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));
GFXcanvas1 shotCanvas(200, 200);  // an in-memory copy of a page, for serial screenshots

// ---- I2C (RTC + sensor) ----
SemaphoreHandle_t i2cLock;
struct I2cGuard {
  I2cGuard() { xSemaphoreTake(i2cLock, portMAX_DELAY); }
  ~I2cGuard() { xSemaphoreGive(i2cLock); }
};
static uint8_t bcd2(uint8_t v) { return (v & 0x0F) + 10 * (v >> 4); }
static uint8_t toBcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

bool readRTC(time_t *out) {
  I2cGuard guard;
  Wire.beginTransmission(RTC_ADDR);
  Wire.write(0x04);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(RTC_ADDR, 7) != 7) return false;
  uint8_t s = Wire.read(), mi = Wire.read(), h = Wire.read(), d = Wire.read(), wd = Wire.read(), mo = Wire.read(), y = Wire.read();
  (void)wd;
  if (s & 0x80) return false;  // oscillator-stop flag: time not trustworthy
  int year = 2000 + bcd2(y);
  if (year < 2025) return false;
  int64_t days = daysFromCivil(year, bcd2(mo & 0x1F), bcd2(d & 0x3F));
  *out = (time_t)(days * 86400 + bcd2(h & 0x3F) * 3600 + bcd2(mi & 0x7F) * 60 + bcd2(s & 0x7F));
  return true;
}

// Called by clock_net.h after an NTP sync, and by the serial T command. Also remembers
// that the RTC now holds a real time (a factory-fresh RTC reports an arbitrary date).
void writeRTC(time_t t) {
  {
    I2cGuard guard;
    struct tm u;
    gmtime_r(&t, &u);
    Wire.beginTransmission(RTC_ADDR);
    Wire.write(0x04);
    Wire.write(toBcd(u.tm_sec));
    Wire.write(toBcd(u.tm_min));
    Wire.write(toBcd(u.tm_hour));
    Wire.write(toBcd(u.tm_mday));
    Wire.write(toBcd(u.tm_wday));
    Wire.write(toBcd(u.tm_mon + 1));
    Wire.write(toBcd(u.tm_year - 100));
    Wire.endTransmission();
  }
  Preferences p;
  p.begin("clock", false);
  p.putBool("rtcset", true);
  p.end();
}

static void setSystemTime(time_t t) {
  struct timeval tv = {t, 0};
  settimeofday(&tv, NULL);
}

#include "clock_net.h"

// ---- sensor ----
float tempOffsetC = 0;
float curT = 0, curRH = 0;
bool curValid = false;

static bool readSHTC3(float &t, float &rh) {
  I2cGuard guard;
  Wire.beginTransmission(SHTC3_ADDR); Wire.write(0x35); Wire.write(0x17);  // wake
  if (Wire.endTransmission() != 0) return false;
  delay(2);
  Wire.beginTransmission(SHTC3_ADDR); Wire.write(0x78); Wire.write(0x66);  // normal mode, T first
  if (Wire.endTransmission() != 0) return false;
  delay(20);
  if (Wire.requestFrom(SHTC3_ADDR, 6) != 6) return false;
  uint16_t rt = Wire.read() << 8; rt |= Wire.read(); Wire.read();
  uint16_t rr = Wire.read() << 8; rr |= Wire.read(); Wire.read();
  Wire.beginTransmission(SHTC3_ADDR); Wire.write(0xB0); Wire.write(0x98); Wire.endTransmission();  // sleep
  t = -45.0f + 175.0f * rt / 65536.0f;
  rh = 100.0f * rr / 65536.0f;
  return true;
}

// ---- battery ----
int battMv = 0;  // millivolts at the cell; below ~2.5 V means no battery is connected
static int readBatteryMv() {
  analogSetPinAttenuation(VBAT_ADC, ADC_11db);
  uint32_t sum = 0;
  for (int i = 0; i < 8; i++) { sum += analogReadMilliVolts(VBAT_ADC); delay(2); }
  return (int)(sum / 8) * 2;
}
// Rough Li-ion open-circuit curve. It reads high while charging and sags under load.
static int battPercent(int mv) {
  static const int V[] = {3300, 3500, 3600, 3700, 3800, 3900, 4000, 4100, 4200};
  static const int P[] = {0, 5, 12, 25, 45, 62, 78, 90, 100};
  if (mv <= V[0]) return 0;
  if (mv >= V[8]) return 100;
  for (int i = 1; i < 9; i++)
    if (mv <= V[i]) return P[i - 1] + (P[i] - P[i - 1]) * (mv - V[i - 1]) / (V[i] - V[i - 1]);
  return 100;
}

// The sensor sits next to the SoC and reads warm. tempOffsetC (serial O command) corrects the
// temperature; relative humidity is then re-derived for the corrected temperature, because the
// same amount of water vapour is a higher percentage in cooler air (Magnus formula).
static float satPressure(float tc) { return 6.112f * expf(17.62f * tc / (243.12f + tc)); }
static void updateSensor() {
  float t, rh;
  if (readSHTC3(t, rh)) {
    float ambient = t + tempOffsetC;
    float vapour = rh / 100.0f * satPressure(t);
    curT = ambient;
    curRH = constrain(100.0f * vapour / satPressure(ambient), 0.0f, 100.0f);
    curValid = true;
  }
  battMv = readBatteryMv();
}

// ---- microSD CSV history ----
bool sdOk = false;
static void sdBegin() {
  SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0);
  sdOk = SD_MMC.begin("/sdcard", true, false);  // 1-bit mode; never formats the card
  if (sdOk) SD_MMC.mkdir("/climate");
  Serial.printf("sd: %s\n", sdOk ? "mounted" : "not available");
}

static void sdLog(time_t t, float tc, float rh) {
  if (!sdOk) sdBegin();
  if (!sdOk) return;
  File f = SD_MMC.open("/climate/log.csv", FILE_APPEND);
  if (!f) { sdOk = false; return; }
  if (f.size() == 0) f.println("local_time,epoch_utc,temp_c,humidity_pct");
  struct tm lt;
  localtime_r(&t, &lt);
  char ts[24];
  strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &lt);
  f.printf("%s,%lu,%.2f,%.1f\n", ts, (unsigned long)t, tc, rh);
  f.close();
}

static void takeSample(time_t now) {
  updateSensor();
  if (!curValid) return;
  LogRec r = {(uint32_t)now, (int16_t)lroundf(curT * 100), (uint16_t)lroundf(curRH * 100)};
  logAppend(r);
  logCompact((uint32_t)now);
  sdLog(now, curT, curRH);
}

// ---- tides: runs on the network task while Wi-Fi is up ----
void onOnline() {
  if (tideStation.length() == 0) return;
  time_t now = time(nullptr);
  bool need = tideStale || strcmp(tideLoadedStation, tideStation.c_str()) != 0 || tidesCoverageHours(now) < 72;
  if (need && tidesFetch(tideStation) > 0) tideStale = false;
  if (tideName.length() == 0) {
    String nm;
    if (tidesFetchName(tideStation, nm)) { tideName = nm; saveTideName(nm); }
  }
  settingsDirty = true;  // new data: redraw
}

// ---- drawing helpers ----
static int textW(Adafruit_GFX &g, const GFXfont *f, const char *s) {
  int16_t x1, y1;
  uint16_t w, h;
  g.setFont(f);
  g.setTextSize(1);
  g.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  return w;
}
static void printAt(Adafruit_GFX &g, const GFXfont *f, int x, int y, const char *s) {
  g.setFont(f);
  g.setTextSize(1);
  g.setTextColor(INK);
  g.setCursor(x, y);
  g.print(s);
}
static void printCenter(Adafruit_GFX &g, const GFXfont *f, int cx, int y, const char *s) {
  int16_t x1, y1;
  uint16_t w, h;
  g.setFont(f);
  g.setTextSize(1);
  g.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  g.setTextColor(INK);
  g.setCursor(cx - (int)w / 2 - x1, y);
  g.print(s);
}
static void printRight(Adafruit_GFX &g, const GFXfont *f, int xr, int y, const char *s) {
  int16_t x1, y1;
  uint16_t w, h;
  g.setFont(f);
  g.setTextSize(1);
  g.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  g.setTextColor(INK);
  g.setCursor(xr - (int)w - x1, y);
  g.print(s);
}
static void hline(Adafruit_GFX &g, int y) { g.drawFastHLine(8, y, 184, INK); }

static float dispTemp(float c) { return useFahrenheit ? c * 9.0f / 5.0f + 32.0f : c; }
static float dispLen(float m) { return useFahrenheit ? m * 3.28084f : m; }

// "9:38 AM" / "09:38", or short "9:38a" for tight spaces.
static void fmtClock(time_t t, char *out, size_t n, bool shortForm) {
  struct tm lt;
  localtime_r(&t, &lt);
  if (hour24Format) {
    snprintf(out, n, "%02d:%02d", lt.tm_hour, lt.tm_min);
    return;
  }
  int h = lt.tm_hour % 12;
  if (h == 0) h = 12;
  bool am = lt.tm_hour < 12;
  if (shortForm) snprintf(out, n, "%d:%02d%c", h, lt.tm_min, am ? 'a' : 'p');
  else snprintf(out, n, "%d:%02d %s", h, lt.tm_min, am ? "AM" : "PM");
}

// ---- moon ----
static void drawMoon(Adafruit_GFX &g, int cx, int cy, int r, double elongDeg) {
  double th = elongDeg * M_PI / 180.0, c = cos(th);
  bool waxing = elongDeg < 180.0;
  for (int y = -r; y <= r; y++) {
    int w = (int)floor(sqrt((double)(r * r - y * y)) + 0.5);
    g.drawFastHLine(cx - w, cy + y, 2 * w + 1, INK);  // whole disc dark first
    int a, b;  // lit span
    if (waxing) { a = (int)lround(w * c); b = w; }
    else { a = -w; b = (int)lround(-w * c); }
    if (b >= a) g.drawFastHLine(cx + a, cy + y, b - a + 1, PAPER);
  }
  g.drawCircle(cx, cy, r, INK);
}

// ---- demo view (serial V): draws synthetic tides and a synthetic week without touching real data ----
static bool demoView = false;
static int demoTides(TideEv *out, time_t now) {
  int n = 0;
  float period = 12.42f * 3600.0f;  // a semidiurnal tide: highs and lows about 6.2 h apart
  time_t t0 = now - 14 * 3600;
  for (int k = 0; k < 40; k++) {
    bool hi = k % 2 == 0;
    out[n++] = {(uint32_t)(t0 + (time_t)(k * period / 2)), hi ? 3.3f + 0.4f * sinf(k * 0.7f) : 0.4f + 0.3f * sinf(k * 0.9f), hi ? 'H' : 'L'};
  }
  return n;
}
static int viewTides(TideEv *out, time_t now) { return demoView ? demoTides(out, now) : tidesSnapshot(out, MAX_TIDES); }

// ---- page 1: clock ----
static void drawClockPage(Adafruit_GFX &g) {
  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);
  bool have = timeSource != TS_NONE;

  char hhmm[8];
  if (have) {
    int h = hour24Format ? lt.tm_hour : (lt.tm_hour % 12 == 0 ? 12 : lt.tm_hour % 12);
    snprintf(hhmm, sizeof hhmm, hour24Format ? "%02d:%02d" : "%d:%02d", h, lt.tm_min);
  } else {
    strcpy(hhmm, "--:--");
  }
  const char *ap = (have && !hour24Format) ? (lt.tm_hour >= 12 ? "PM" : "AM") : "";
  int tw = textW(g, &FreeSansBold24pt7b, hhmm);
  int aw = *ap ? textW(g, &FreeSans9pt7b, ap) + 4 : 0;
  int x0 = (200 - tw - aw) / 2;
  printAt(g, &FreeSansBold24pt7b, x0, 46, hhmm);
  if (*ap) printAt(g, &FreeSans9pt7b, x0 + tw + 4, 46, ap);

  char buf[40];
  if (have) {
    char wd[16], mo[8];
    strftime(wd, sizeof wd, "%A", &lt);
    strftime(mo, sizeof mo, "%b", &lt);
    snprintf(buf, sizeof buf, "%s, %s %d", wd, mo, lt.tm_mday);
  } else {
    strcpy(buf, "Setting the time...");
  }
  printCenter(g, &FreeSans9pt7b, 100, 66, buf);
  hline(g, 74);

  // temperature (left) and humidity (right)
  char tv[12], hv[12];
  if (curValid) {
    snprintf(tv, sizeof tv, "%.1f", dispTemp(curT));
    snprintf(hv, sizeof hv, "%.0f%%", curRH);
  } else {
    strcpy(tv, "--");
    strcpy(hv, "--%");
  }
  const char *unit = useFahrenheit ? "F" : "C";
  int tW = textW(g, &FreeSansBold12pt7b, tv), uW = textW(g, &FreeSansBold12pt7b, unit);
  int block = tW + 8 + uW;
  int tx = 50 - block / 2;
  printAt(g, &FreeSansBold12pt7b, tx, 98, tv);
  g.drawCircle(tx + tW + 4, 84, 2, INK);
  printAt(g, &FreeSansBold12pt7b, tx + tW + 8, 98, unit);
  printCenter(g, &FreeSansBold12pt7b, 148, 98, hv);
  g.drawFastVLine(100, 82, 20, INK);
  hline(g, 108);

  // next two tide events
  TideEv ev[MAX_TIDES];
  int n = viewTides(ev, now), shown = 0;
  for (int i = 0; i < n && shown < 2; i++) {
    if ((time_t)ev[i].t < now) continue;
    int y = 126 + shown * 18;
    if (ev[i].type == 'H') g.fillTriangle(8, y - 1, 16, y - 1, 12, y - 9, INK);
    else g.fillTriangle(8, y - 9, 16, y - 9, 12, y - 1, INK);
    char tm[12], line[40];
    fmtClock((time_t)ev[i].t, tm, sizeof tm, false);
    snprintf(line, sizeof line, "%s %s  %.1f %s", ev[i].type == 'H' ? "High" : "Low", tm, dispLen(ev[i].m), useFahrenheit ? "ft" : "m");
    printAt(g, &FreeSans9pt7b, 22, y, line);
    shown++;
  }
  if (shown == 0) printAt(g, &FreeSans9pt7b, 8, 134, n ? "Tides: refreshing..." : "Tides: waiting for Wi-Fi");
  hline(g, 152);

  // moon
  double e = moonElong(have ? now : 0);
  drawMoon(g, 30, 176, 20, e);
  char lit[24];
  snprintf(lit, sizeof lit, "%.0f%% lit", moonIllum(e) * 100.0f);
  printAt(g, &FreeSans9pt7b, 58, 170, moonName(e));
  g.setFont();
  g.setTextSize(1);
  g.setTextColor(INK);
  g.setCursor(58, 177);
  g.print(lit);
  time_t nf = moonNext(now, 180.0), nn = moonNext(now, 0.0);
  bool fullSooner = nf && (!nn || nf < nn);
  time_t nt = fullSooner ? nf : nn;
  if (nt && have) {
    struct tm mt;
    localtime_r(&nt, &mt);
    char mo[8], l2[40];
    strftime(mo, sizeof mo, "%b", &mt);
    snprintf(l2, sizeof l2, "%s moon %s %d", fullSooner ? "Full" : "New", mo, mt.tm_mday);
    g.setCursor(58, 188);
    g.print(l2);
  }
}

// ---- page 2: 7-day climate plot ----
static LogRec plotBuf[1200];
static float serT[168], serH[168];
static bool serOkT[168], serOkH[168];

static void plotSeries(Adafruit_GFX &g, int x0, int y0, int w, int h, const float *v, const bool *ok, bool isTemp,
                       int64_t startHour, time_t now, const char *title, const char *curText) {
  printAt(g, &FreeSans9pt7b, 6, y0 - 4, title);
  printRight(g, &FreeSans9pt7b, 194, y0 - 4, curText);
  float lo = 1e9, hi = -1e9;
  for (int i = 0; i < w; i++) if (ok[i]) { lo = min(lo, v[i]); hi = max(hi, v[i]); }
  g.drawFastVLine(x0 - 1, y0, h, INK);
  g.drawFastHLine(x0 - 1, y0 + h, w + 1, INK);
  if (lo > hi) {
    printCenter(g, &FreeSans9pt7b, x0 + w / 2, y0 + h / 2 + 4, "no data yet");
    return;
  }
  float span = isTemp ? 8.0f : 20.0f;
  lo = floorf(lo - (isTemp ? 0.5f : 1.0f));
  hi = ceilf(hi + (isTemp ? 0.5f : 1.0f));
  if (hi - lo < span) {
    float mid = (hi + lo) * 0.5f;
    lo = floorf(mid - span / 2);
    hi = lo + span;
  }
  if (!isTemp) { if (lo < 0) { lo = 0; hi = max(hi, span); } if (hi > 100) { hi = 100; lo = min(lo, 100 - span); } }

  // day boundaries (local midnights): dotted verticals + weekday initials
  struct tm lt;
  localtime_r(&now, &lt);
  for (int d = 0; d <= 7; d++) {
    struct tm mt = lt;
    mt.tm_hour = 0; mt.tm_min = 0; mt.tm_sec = 0; mt.tm_mday -= d; mt.tm_isdst = -1;
    time_t m = mktime(&mt);
    int px = (int)(m / 3600 - startHour);
    if (px >= 0 && px < w) {
      for (int y = y0; y < y0 + h; y += 3) g.drawPixel(x0 + px, y, INK);
      g.drawFastVLine(x0 + px, y0 + h, 3, INK);
    }
    int lx = x0 + px + 12 - 3;
    if (lx >= x0 && lx + 6 <= x0 + w) {
      g.setFont();
      g.setTextSize(1);
      g.setTextColor(INK);
      g.setCursor(lx, y0 + h + 4);
      g.print("SMTWTFS"[mt.tm_wday]);
    }
  }
  for (int x = x0; x < x0 + w; x += 4) g.drawPixel(x, y0 + h / 2, INK);  // dotted middle gridline

  // axis range labels
  char a[8];
  g.setFont();
  g.setTextSize(1);
  g.setTextColor(INK);
  snprintf(a, sizeof a, "%.0f", hi);
  g.setCursor(x0 - 3 - 6 * strlen(a), y0);
  g.print(a);
  snprintf(a, sizeof a, "%.0f", lo);
  g.setCursor(x0 - 3 - 6 * strlen(a), y0 + h - 8);
  g.print(a);

  auto yOf = [&](float val) { return y0 + h - 1 - (int)lroundf((val - lo) / (hi - lo) * (h - 1)); };
  for (int i = 0; i < w; i++) {
    if (!ok[i]) continue;
    int y = yOf(v[i]);
    if (i + 1 < w && ok[i + 1]) {
      g.drawLine(x0 + i, y, x0 + i + 1, yOf(v[i + 1]), INK);
    } else if (i + 2 < w && ok[i + 2]) {
      g.drawLine(x0 + i, y, x0 + i + 2, yOf(v[i + 2]), INK);  // bridge a one-column gap
    } else {
      g.drawPixel(x0 + i, y, INK);
    }
  }
}

static void drawClimatePage(Adafruit_GFX &g) {
  time_t now = time(nullptr);
  int64_t endHour = now / 3600, startHour = endHour - 167;
  size_t n = (timeSource == TS_NONE) ? 0 : logLoad((uint32_t)(startHour * 3600), plotBuf, 1200);
  static float sT[168], sH[168];
  static int cnt[168];
  memset(sT, 0, sizeof sT);
  memset(sH, 0, sizeof sH);
  memset(cnt, 0, sizeof cnt);
  for (size_t i = 0; i < n; i++) {
    int b = (int)(plotBuf[i].t / 3600 - startHour);
    if (b < 0 || b > 167) continue;
    sT[b] += plotBuf[i].tc / 100.0f;
    sH[b] += plotBuf[i].rh / 100.0f;
    cnt[b]++;
  }
  if (demoView) {  // a believable week: daily swing peaking mid-afternoon, plus a slow weekly drift
    for (int b = 0; b < 168; b++) {
      time_t tt = (time_t)((startHour + b) * 3600);
      struct tm dl;
      localtime_r(&tt, &dl);
      float hod = dl.tm_hour, day = b / 24.0f;
      sT[b] = 20.5f + 2.6f * sinf(2 * M_PI * (hod - 16) / 24.0f) + 1.3f * sinf(2 * M_PI * day / 7.0f) + 0.15f * sinf(b * 1.7f);
      sH[b] = 46 - 7 * sinf(2 * M_PI * (hod - 16) / 24.0f) + 4 * cosf(2 * M_PI * day / 5.0f) + 0.7f * sinf(b * 2.3f);
      cnt[b] = 1;
    }
  }
  for (int i = 0; i < 168; i++) {
    serOkT[i] = serOkH[i] = cnt[i] > 0;
    serT[i] = cnt[i] ? dispTemp(sT[i] / cnt[i]) : 0;
    serH[i] = cnt[i] ? sH[i] / cnt[i] : 0;
  }
  char ct[16], ch[16];
  if (curValid) {
    snprintf(ct, sizeof ct, "%.1f %s", dispTemp(curT), useFahrenheit ? "F" : "C");
    snprintf(ch, sizeof ch, "%.0f%%", curRH);
  } else {
    strcpy(ct, "--");
    strcpy(ch, "--");
  }
  plotSeries(g, 26, 17, 168, 66, serT, serOkT, true, startHour, now, "Temp, 7 days", ct);
  plotSeries(g, 26, 119, 168, 66, serH, serOkH, false, startHour, now, "Humidity, 7 days", ch);
}

// ---- page 3: tides ----
static void drawTidePage(Adafruit_GFX &g) {
  time_t now = time(nullptr);
  TideEv ev[MAX_TIDES];
  int n = viewTides(ev, now);
  printAt(g, &FreeSans9pt7b, 6, 14, "Tides");
  g.setFont();
  g.setTextSize(1);
  g.setTextColor(INK);
  g.setCursor(6, 20);
  g.print(demoView ? "Demo Harbor" : (tideName.length() ? tideName.c_str() : tideStation.c_str()));
  float cover = n ? ((int64_t)ev[n - 1].t - (int64_t)now) / 3600.0f : 0;
  if (n < 2 || timeSource == TS_NONE || cover < 6) {
    printCenter(g, &FreeSans9pt7b, 100, 100, n < 2 ? "No tide data yet" : "Tide data expired");
    printCenter(g, &FreeSans9pt7b, 100, 120, "needs Wi-Fi");
    return;
  }
  const int x0 = 22, y0 = 38, w = 172, h = 96;
  time_t ws = now - 6 * 3600, we = now + 18 * 3600;
  static float hv[172];
  static bool hok[172];
  float lo = 1e9, hi = -1e9;
  for (int i = 0; i < w; i++) {
    time_t t = ws + (time_t)((int64_t)i * (we - ws) / (w - 1));
    float hm;
    hok[i] = tideHeightAt(ev, n, t, hm);
    hv[i] = hok[i] ? dispLen(hm) : 0;
    if (hok[i]) { lo = min(lo, hv[i]); hi = max(hi, hv[i]); }
  }
  if (lo > hi) { lo = 0; hi = 1; }
  lo = floorf(lo - 0.5f);
  hi = ceilf(hi + 0.5f);
  auto yOf = [&](float val) { return y0 + h - 1 - (int)lroundf((val - lo) / (hi - lo) * (h - 1)); };
  g.drawFastVLine(x0 - 1, y0, h, INK);
  g.drawFastHLine(x0 - 1, y0 + h, w + 1, INK);
  // water: dotted fill under the curve, then the curve itself
  for (int i = 0; i < w; i++) {
    if (!hok[i]) continue;
    for (int y = yOf(hv[i]) + 1; y < y0 + h; y++)
      if (((x0 + i) + y) % 4 == 0) g.drawPixel(x0 + i, y, INK);
  }
  for (int i = 0; i + 1 < w; i++)
    if (hok[i] && hok[i + 1]) {
      g.drawLine(x0 + i, yOf(hv[i]), x0 + i + 1, yOf(hv[i + 1]), INK);
      g.drawLine(x0 + i, yOf(hv[i]) - 1, x0 + i + 1, yOf(hv[i + 1]) - 1, INK);
    }
  // "now" marker
  int nx = x0 + w * 6 / 24;
  for (int y = y0; y < y0 + h; y += 4) g.drawFastVLine(nx, y, 2, INK);
  g.fillTriangle(nx - 3, y0 + h + 6, nx + 3, y0 + h + 6, nx, y0 + h + 1, INK);
  // range labels
  g.setFont();
  g.setTextSize(1);
  g.setTextColor(INK);
  char a[8];
  snprintf(a, sizeof a, "%.0f", hi);
  g.setCursor(x0 - 3 - 6 * strlen(a), y0);
  g.print(a);
  snprintf(a, sizeof a, "%.0f", lo);
  g.setCursor(x0 - 3 - 6 * strlen(a), y0 + h - 8);
  g.print(a);
  // hour ticks every 6 hours
  struct tm lt;
  for (time_t t = (ws / 3600 + 1) * 3600; t < we; t += 3600) {
    localtime_r(&t, &lt);
    if (lt.tm_hour % 6) continue;
    int x = x0 + (int)((int64_t)(t - ws) * (w - 1) / (we - ws));
    g.drawFastVLine(x, y0 + h, 3, INK);
    char l[6];
    if (hour24Format) snprintf(l, sizeof l, "%d", lt.tm_hour);
    else snprintf(l, sizeof l, "%d%c", lt.tm_hour % 12 == 0 ? 12 : lt.tm_hour % 12, lt.tm_hour < 12 ? 'a' : 'p');
    g.setCursor(x - 3 * strlen(l), y0 + h + 5);
    g.print(l);
  }
  // events inside the window: dots + short time labels
  for (int i = 0; i < n; i++) {
    if ((time_t)ev[i].t < ws || (time_t)ev[i].t > we) continue;
    int x = x0 + (int)((int64_t)((time_t)ev[i].t - ws) * (w - 1) / (we - ws));
    int y = yOf(dispLen(ev[i].m));
    g.fillCircle(x, y, 2, PAPER);
    g.drawCircle(x, y, 3, INK);
    char tm[10];
    fmtClock((time_t)ev[i].t, tm, sizeof tm, true);
    int lw = 6 * (int)strlen(tm);
    int lx = constrain(x - lw / 2, x0, x0 + w - lw);
    int ly = ev[i].type == 'H' ? max(y0 + 1, y - 14) : min(y0 + h - 10, y + 7);
    g.fillRect(lx - 1, ly - 1, lw + 2, 10, PAPER);
    g.setCursor(lx, ly);
    g.print(tm);
  }
  // next two events
  int shown = 0;
  for (int i = 0; i < n && shown < 2; i++) {
    if ((time_t)ev[i].t < now) continue;
    int y = 168 + shown * 20;
    if (ev[i].type == 'H') g.fillTriangle(8, y - 1, 16, y - 1, 12, y - 9, INK);
    else g.fillTriangle(8, y - 9, 16, y - 9, 12, y - 1, INK);
    char tm[12], line[40];
    fmtClock((time_t)ev[i].t, tm, sizeof tm, false);
    snprintf(line, sizeof line, "%s %s  %.1f %s", ev[i].type == 'H' ? "High" : "Low", tm, dispLen(ev[i].m), useFahrenheit ? "ft" : "m");
    printAt(g, &FreeSans9pt7b, 22, y, line);
    shown++;
  }
}

// ---- battery icon (top right of the clock page; hidden when no battery is connected) ----
static void drawBattery(Adafruit_GFX &g) {
  if (battMv < 2500) return;
  int pc = battPercent(battMv);
  g.drawRect(176, 3, 19, 9, INK);
  g.fillRect(195, 5, 2, 5, INK);
  int w = (15 * pc + 50) / 100;
  if (w > 0) g.fillRect(178, 5, w, 5, INK);
  char t[6];
  snprintf(t, sizeof t, "%d%%", pc);
  g.setFont();
  g.setTextSize(1);
  g.setTextColor(INK);
  g.setCursor(176 - 6 * (int)strlen(t) - 3, 4);
  g.print(t);
}

// ---- power-off / empty-battery message (e-paper keeps it on screen with no power) ----
static const char *offLine1 = "Powered off";
static const char *offLine2 = "Press PWR to start";
static void drawOffPage(Adafruit_GFX &g) {
  printCenter(g, &FreeSansBold12pt7b, 100, 92, offLine1);
  printCenter(g, &FreeSans9pt7b, 100, 120, offLine2);
}

// ---- Wi-Fi setup screen ----
static Adafruit_GFX *qrTarget = nullptr;
static void qrDraw(esp_qrcode_handle_t qr) {
  Adafruit_GFX &g = *qrTarget;
  const int n = esp_qrcode_get_size(qr);
  int mod = min(4, 134 / (n + 4));
  const int box = (n + 4) * mod, x0 = 100 - box / 2, y0 = 26;
  g.fillRect(x0, y0, box, box, PAPER);
  g.drawRect(x0 - 1, y0 - 1, box + 2, box + 2, INK);
  for (int y = 0; y < n; y++)
    for (int x = 0; x < n; x++)
      if (esp_qrcode_get_module(qr, x, y)) g.fillRect(x0 + (x + 2) * mod, y0 + (y + 2) * mod, mod, mod, INK);
}

static void drawSetupPage(Adafruit_GFX &g) {
  printCenter(g, &FreeSansBold12pt7b, 100, 20, "Wi-Fi setup");
  qrTarget = &g;
  esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
  cfg.display_func = qrDraw;
  cfg.max_qrcode_version = 6;
  esp_qrcode_generate(&cfg, "WIFI:T:nopass;S:" SETUP_AP_SSID ";;");
  g.setFont();
  g.setTextSize(1);
  g.setTextColor(INK);
  g.setCursor(4, 167);
  g.print("Scan, or join " SETUP_AP_SSID);
  const char *msg;
  switch (setupStatus) {
    case 1: msg = "Phone connected"; break;
    case 2: msg = "Connecting..."; break;
    case 3: msg = "All set!"; break;
    case 4: msg = "Could not connect"; break;
    default: msg = "Waiting for phone..."; break;
  }
  printCenter(g, &FreeSans9pt7b, 100, 190, msg);
}

// ---- pages ----
enum { PG_CLOCK = 0, PG_CLIMATE = 1, PG_TIDES = 2, PG_COUNT = 3, PG_OFF = 8, PG_SETUP = 9 };
static int page = PG_CLOCK;

static void drawPage(Adafruit_GFX &g, int pg) {
  g.setRotation(ROTATION);
  g.fillScreen(PAPER);
  if (pg == PG_SETUP) drawSetupPage(g);
  else if (pg == PG_OFF) drawOffPage(g);
  else if (pg == PG_CLIMATE) drawClimatePage(g);
  else if (pg == PG_TIDES) drawTidePage(g);
  else {
    drawClockPage(g);
    drawBattery(g);
  }
}

static void render(int pg, bool full) {
  if (full) display.setFullWindow();
  else display.setPartialWindow(0, 0, 200, 200);
  display.firstPage();
  do { drawPage(display, pg); } while (display.nextPage());
}

// Shows a message, then releases the battery latch. On a battery the board powers down and the
// e-paper keeps the message; on USB the board stays up, so it just carries on.
static void powerOff(const char *l1, const char *l2) {
  Serial.printf("power off: %s\n", l1);
  offLine1 = l1;
  offLine2 = l2;
  render(PG_OFF, true);
  delay(500);
  digitalWrite(VBAT_LATCH, LOW);
  delay(2500);
  settingsDirty = true;
}

// Serial screenshot of what a page looks like (e-paper cannot be read back, so this draws the
// same page into an in-memory canvas): "SHOT", byte count, then 200x200 1-bit rows (1 = white).
static void sendShot(int pg) {
  shotCanvas.setRotation(0);
  drawPage(shotCanvas, pg);
  uint32_t hdr[2] = {0x544F4853u, 5000};
  Serial.write((const uint8_t *)hdr, sizeof hdr);
  const uint8_t *buf = shotCanvas.getBuffer();
  uint32_t giveUp = millis() + 3000;
  for (size_t off = 0; off < 5000 && millis() < giveUp;) {
    size_t k = Serial.write(buf + off, min((size_t)256, 5000 - off));
    off += k;
    if (k) giveUp = millis() + 3000;
    else delay(1);
  }
}

// ---- demo data (serial F): a believable week of readings and a tide cycle, for screenshots ----
static void fillDemo() {
  time_t now = time(nullptr);
  logClear();
  File f = LittleFS.open(LOG_PATH, "w");
  uint32_t start = (uint32_t)(now / 600) * 600 - 1007UL * 600;
  for (int k = 0; k < 1008; k++) {
    uint32_t t = start + k * 600UL;
    struct tm lt;
    time_t tt = t;
    localtime_r(&tt, &lt);
    float hod = lt.tm_hour + lt.tm_min / 60.0f;
    float day = (t - start) / 86400.0f;
    float tc = 20.5f + 2.6f * sinf(2 * M_PI * (hod - 16) / 24.0f) + 1.3f * sinf(2 * M_PI * day / 7.0f) + (esp_random() % 30) / 100.0f;
    float rh = 46 - 7 * sinf(2 * M_PI * (hod - 16) / 24.0f) + 4 * cosf(2 * M_PI * day / 5.0f) + (esp_random() % 100) / 50.0f;
    LogRec r = {t, (int16_t)lroundf(tc * 100), (uint16_t)lroundf(rh * 100)};
    f.write((const uint8_t *)&r, sizeof r);
  }
  f.close();
  static TideEv ev[40];
  int n = 0;
  float period = 12.42f * 3600.0f;  // a semidiurnal tide: highs and lows about 6.2 h apart
  time_t t0 = now - 14 * 3600;
  for (int k = 0; k < 40; k++) {
    bool hi = k % 2 == 0;
    ev[n++] = {(uint32_t)(t0 + (time_t)(k * period / 2)), hi ? 3.3f + 0.4f * sinf(k * 0.7f) : 0.4f + 0.3f * sinf(k * 0.9f), hi ? 'H' : 'L'};
  }
  tidesSet(ev, n, "demo");
  tideName = "Demo Harbor, WA";
}

// ---- serial commands ----
static bool shotRequested = false;
static int shotPage = 0;
static bool pageChangeRequested = false;

static void requestSetup() {
  setupRequested = true;
  if (netTaskHandle) xTaskNotifyGive(netTaskHandle);
  Serial.println("setup requested");
}

static void handleSerial() {
  static String line;
  while (Serial.available()) {
    char c = Serial.read();
    if (c != '\n' && c != '\r') {
      if (line.length() < 120) line += c;
      continue;
    }
    if (line.length() == 0) continue;
    char k = line[0];
    String arg = line.substring(1);
    if (k == 'S') {
      shotPage = arg.length() ? arg.toInt() : page;
      shotRequested = true;
    } else if (k == 'N') {
      page = (page + 1) % PG_COUNT;
      pageChangeRequested = true;
    } else if (k == 'B') {
      page = (page + PG_COUNT - 1) % PG_COUNT;
      pageChangeRequested = true;
    } else if (k == 'T' && arg.length()) {
      time_t t = (time_t)arg.toInt();
      if (t > 1700000000) {
        setSystemTime(t);
        writeRTC(t);
        timeSource = TS_SYNCED;
        settingsDirty = true;
        Serial.printf("time set to %ld\n", (long)t);
      }
    } else if (k == 'W') {
      int tab = arg.indexOf('\t');
      if (tab > 0) {
        saveCreds(arg.substring(0, tab), arg.substring(tab + 1));
        Serial.printf("wifi saved for '%s'\n", arg.substring(0, tab).c_str());
        if (netTaskHandle) xTaskNotifyGive(netTaskHandle);
      }
    } else if (k == 'X') {
      saveCreds("", "");
      Serial.println("wifi cleared");
    } else if (k == 'P') {
      requestSetup();
    } else if (k == 'R') {
      tideStale = true;
      if (netTaskHandle) xTaskNotifyGive(netTaskHandle);
      Serial.println("refresh requested");
    } else if (k == 'E') {  // E<station id>
      arg.trim();
      tideStation = arg;
      saveTideStation(arg);
      tideName = "";
      tideStale = true;
      settingsDirty = true;
      if (netTaskHandle) xTaskNotifyGive(netTaskHandle);
      Serial.printf("tide station set to %s\n", arg.c_str());
    } else if (k == 'O') {  // O<degrees C offset to subtract/add>
      tempOffsetC = arg.toFloat();
      Preferences p;
      p.begin("clock", false);
      p.putFloat("toff", tempOffsetC);
      p.end();
      Serial.printf("temperature offset %.2f C\n", tempOffsetC);
    } else if (k == 'V') {  // toggle the demo view (screenshots only; nothing is stored)
      demoView = !demoView;
      settingsDirty = true;
      Serial.printf("demo view %s\n", demoView ? "on" : "off");
    } else if (k == 'F') {
      fillDemo();
      settingsDirty = true;
      Serial.println("demo data loaded (Z clears it)");
    } else if (k == 'Z') {
      logClear();
      tidesClear();
      tideName = "";
      tideStale = true;
      settingsDirty = true;
      Serial.println("log and tides cleared");
    } else if (k == 'C') {  // tail of the microSD CSV
      File f = sdOk ? SD_MMC.open("/climate/log.csv") : File();
      if (!f) {
        Serial.println("no csv on the sd card yet");
      } else {
        size_t sz = f.size();
        Serial.printf("csv: %u bytes\n", (unsigned)sz);
        if (sz > 240) f.seek(sz - 240);
        Serial.print(f.readString());
        f.close();
      }
    } else if (k == 'D') {
      static LogRec buf[1200];
      size_t n = logLoad(0, buf, 1200);
      Serial.printf("epoch_utc,temp_c,humidity_pct (%u rows)\n", (unsigned)n);
      for (size_t i = 0; i < n; i++) Serial.printf("%lu,%.2f,%.1f\n", (unsigned long)buf[i].t, buf[i].tc / 100.0f, buf[i].rh / 100.0f);
    } else if (k == '?') {
      String ssid, pass;
      bool have = loadCreds(ssid, pass);
      Serial.printf("time source %d, tz %s, %s, %s, wifi %s, tide %s (%s), log %u rows, sd %s, temp %.1fC %.0f%%, battery %d mV (%d%%)\n", (int)timeSource,
                    tzString.c_str(), hour24Format ? "24h" : "12h", useFahrenheit ? "F" : "C", have ? ssid.c_str() : "(none)",
                    tideStation.c_str(), tideName.c_str(), (unsigned)logCount(), sdOk ? "ok" : "none", curT, curRH, battMv, battMv < 2500 ? 0 : battPercent(battMv));
    }
    line = "";
  }
}

// ---- buttons ----
static void pollButtons() {
  static const int pins[2] = {BOOT_BTN, PWR_BTN};
  static uint32_t downAt[2] = {0, 0};
  static bool down[2] = {false, false}, fired[2] = {false, false};
  static bool started = false;
  uint32_t t = millis();
  if (!started) {  // a button already held at boot (PWR powers the board on) must not count as a press
    started = true;
    for (int i = 0; i < 2; i++)
      if (digitalRead(pins[i]) == LOW) { down[i] = true; fired[i] = true; downAt[i] = t; }
  }
  for (int i = 0; i < 2; i++) {
    bool pressed = digitalRead(pins[i]) == LOW;
    if (pressed && !down[i]) {
      down[i] = true;
      downAt[i] = t;
      fired[i] = false;
    } else if (pressed && !fired[i] && t - downAt[i] > 2000) {
      fired[i] = true;
      if (i == 0 && !setupActive) requestSetup();
      if (i == 1 && !setupActive) powerOff("Powered off", "Press PWR to start");
    } else if (!pressed && down[i]) {
      down[i] = false;
      if (!fired[i] && t - downAt[i] > 40 && !setupActive) {
        page = i == 0 ? (page + 1) % PG_COUNT : (page + PG_COUNT - 1) % PG_COUNT;
        pageChangeRequested = true;
        Serial.printf("%s short press -> page %d\n", i == 0 ? "BOOT" : "PWR", page);
      }
    }
  }
}

// ---- main ----
void setup() {
  pinMode(VBAT_LATCH, OUTPUT); digitalWrite(VBAT_LATCH, HIGH);  // first: keeps a battery-started board alive
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);  // never stall the display if nothing is reading the port
  setCpuFrequencyMhz(80);    // plenty for a clock, and the chip runs cooler next to the sensor
  delay(1500);
  i2cLock = xSemaphoreCreateMutex();
  pinMode(VBAT_LATCH, OUTPUT); digitalWrite(VBAT_LATCH, HIGH);
  pinMode(EPD_PWR, OUTPUT); digitalWrite(EPD_PWR, LOW);
  pinMode(AUDIO_PWR, OUTPUT); digitalWrite(AUDIO_PWR, LOW);
  pinMode(BOOT_BTN, INPUT_PULLUP);
  pinMode(PWR_BTN, INPUT_PULLUP);
  delay(100);
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(300000);

  logBegin();
  tidesInit();
  applyTz(loadTz());
  hour24Format = loadHour24();
  loadExtraSettings();
  {
    Preferences p;
    p.begin("clock", true);
    tempOffsetC = p.getFloat("toff", 0);
    bool rtcSet = p.getBool("rtcset", false);
    p.end();
    time_t t;
    if (rtcSet && readRTC(&t)) {
      setSystemTime(t);
      timeSource = TS_RTC;
    }
  }
  sdBegin();

  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);
  display.init(0, true, 2, false);
  updateSensor();
  render(PG_CLOCK, true);

  xTaskCreatePinnedToCore(netTask, "net", 12288, NULL, 1, &netTaskHandle, 0);
  Serial.printf("ready; time source %d\n", (int)timeSource);
}

void loop() {
  static int lastMin = -1, partials = 0;
  static bool wasSetup = false, hadTime = false;
  static uint32_t pageSince = 0, lastSetupDraw = 0;
  static int lastSetupStatus = -1;
  handleSerial();
  if (shotRequested) {
    shotRequested = false;
    sendShot(shotPage);
  }

  if (setupActive) {
    wasSetup = true;
    if (lastSetupStatus != setupStatus) {
      lastSetupStatus = setupStatus;
      render(PG_SETUP, true);
    }
    delay(100);
    return;
  }
  pollButtons();
  bool needRender = false, needFull = false;
  if (wasSetup) {
    wasSetup = false;
    lastSetupStatus = -1;
    needRender = needFull = true;
  }
  if (pageChangeRequested) {
    pageChangeRequested = false;
    pageSince = millis();
    needRender = needFull = true;
  }
  if (page != PG_CLOCK && millis() - pageSince > 180000UL) {  // drift back to the clock page
    page = PG_CLOCK;
    needRender = needFull = true;
  }
  if (settingsDirty) {
    settingsDirty = false;
    needRender = needFull = true;
  }

  time_t now = time(nullptr);
  struct tm lt;
  localtime_r(&now, &lt);
  bool haveTime = timeSource != TS_NONE;
  if (haveTime != hadTime) {
    hadTime = haveTime;
    lastMin = -1;  // first valid time: tick immediately
  }
  if (haveTime && lt.tm_min != lastMin) {
    bool firstTick = lastMin < 0;
    lastMin = lt.tm_min;
    bool sampleDue = lt.tm_min % LOG_INTERVAL_MIN == 0 || firstTick;
    if (sampleDue) takeSample(now);
    else updateSensor();
    static int lowCount = 0;
    if (battMv >= 2500 && battMv < 3250) {  // empty cell (a reading under 2.5 V means no battery)
      if (++lowCount >= 3) {
        lowCount = 0;
        powerOff("Battery empty", "Charge, then press PWR");
      }
    } else {
      lowCount = 0;
    }
    if (page == PG_CLOCK) {
      needRender = true;
      if (++partials >= LOG_INTERVAL_MIN) { needFull = true; }
    } else if (sampleDue) {
      needRender = needFull = true;
    }
  }
  if (needRender) {
    if (needFull) partials = 0;
    render(page, needFull);
  }
  delay(40);
}

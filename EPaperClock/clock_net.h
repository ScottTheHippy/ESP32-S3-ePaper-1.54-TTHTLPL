#pragma once
// Wi-Fi and time: NTP sync on every boot and every 6 hours, a stored timezone, and phone-based
// Wi-Fi/timezone setup (the clock hosts a temporary open network with a captive portal).
// The including sketch provides LOCAL_TZ, SETUP_AP_SSID, TIDE_DEFAULT_STATION, writeRTC(time_t)
// and onOnline() (called while Wi-Fi is up right after a successful time sync).
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <esp_sntp.h>

enum TimeSource { TS_NONE, TS_RTC, TS_SYNCED };
volatile TimeSource timeSource = TS_NONE;  // where the current time came from
volatile bool setupActive = false;         // portal running: the main loop shows the setup screen
volatile bool setupRequested = false;      // long-press or serial "P"
volatile bool setupCancel = false;         // tap while the setup screen is up
volatile int setupStatus = 0;              // 0 waiting, 1 phone joined, 2 connecting, 3 done, 4 failed
time_t lastSyncEpoch = 0;
String tzString = LOCAL_TZ;
TaskHandle_t netTaskHandle = nullptr;

struct TzChoice {
  const char *label;
  const char *posix;
};
static const TzChoice TZ_CHOICES[] = {
  {"Pacific (Seattle, Los Angeles)", "PST8PDT,M3.2.0,M11.1.0"},
  {"Mountain (Denver)", "MST7MDT,M3.2.0,M11.1.0"},
  {"Mountain, no DST (Arizona)", "MST7"},
  {"Central (Chicago)", "CST6CDT,M3.2.0,M11.1.0"},
  {"Eastern (New York)", "EST5EDT,M3.2.0,M11.1.0"},
  {"Atlantic (Halifax)", "AST4ADT,M3.2.0,M11.1.0"},
  {"Alaska", "AKST9AKDT,M3.2.0,M11.1.0"},
  {"Hawaii", "HST10"},
  {"UK, Ireland, Portugal", "GMT0BST,M3.5.0/1,M10.5.0"},
  {"Central Europe (Paris, Berlin)", "CET-1CEST,M3.5.0,M10.5.0/3"},
  {"Eastern Europe (Athens, Helsinki)", "EET-2EEST,M3.5.0/3,M10.5.0/4"},
  {"Moscow", "MSK-3"},
  {"India", "IST-5:30"},
  {"China, Singapore, Perth", "CST-8"},
  {"Japan, Korea", "JST-9"},
  {"Sydney, Melbourne", "AEST-10AEDT,M10.1.0,M4.1.0/3"},
  {"New Zealand", "NZST-12NZDT,M9.5.0,M4.1.0/3"},
  {"UTC", "UTC0"},
};
static const int TZ_COUNT = sizeof(TZ_CHOICES) / sizeof(TZ_CHOICES[0]);
bool hour24Format = false;  // set from Preferences at boot (loadHour24) and from the setup page
bool useFahrenheit = true;  // temperature + tide height units
String tideStation = TIDE_DEFAULT_STATION;
String tideName = "";
volatile bool settingsDirty = false;  // a setting changed: the main loop should redraw
volatile bool tideStale = true;       // station changed or never fetched: refetch on the next sync
void onOnline();

// ---------- stored settings ----------
static String loadTz() {
  Preferences p;
  p.begin("clock", true);
  String s = p.getString("tz", LOCAL_TZ);
  p.end();
  return s;
}

static void saveTz(const String &tz) {
  Preferences p;
  p.begin("clock", false);
  p.putString("tz", tz);
  p.end();
}

static void applyTz(const String &tz) {
  tzString = tz;
  setenv("TZ", tz.c_str(), 1);
  tzset();
}

static bool loadHour24() {
  Preferences p;
  p.begin("clock", true);
  bool v = p.getBool("hour24", false);
  p.end();
  return v;
}

static void saveHour24(bool v) {
  Preferences p;
  p.begin("clock", false);
  p.putBool("hour24", v);
  p.end();
}

static void loadExtraSettings() {
  Preferences p;
  p.begin("clock", true);
  useFahrenheit = p.getBool("degF", true);
  tideStation = p.getString("tide", TIDE_DEFAULT_STATION);
  tideName = p.getString("tidename", "");
  p.end();
}

static void saveUnits(bool f) {
  Preferences p;
  p.begin("clock", false);
  p.putBool("degF", f);
  p.end();
}

static void saveTideStation(const String &id) {
  Preferences p;
  p.begin("clock", false);
  p.putString("tide", id);
  p.putString("tidename", "");
  p.end();
}

static void saveTideName(const String &n) {
  Preferences p;
  p.begin("clock", false);
  p.putString("tidename", n);
  p.end();
}

static bool loadCreds(String &ssid, String &pass) {
  Preferences p;
  p.begin("wifi", true);
  ssid = p.getString("ssid", "");
  pass = p.getString("pass", "");
  p.end();
  return ssid.length() > 0;
}

static void saveCreds(const String &ssid, const String &pass) {
  Preferences p;
  p.begin("wifi", false);
  p.putString("ssid", ssid);
  p.putString("pass", pass);
  p.end();
}

// ---------- join Wi-Fi and read the time ----------
static void pauseMs(uint32_t ms, void (*pump)()) {
  if (pump) pump();
  else vTaskDelay(pdMS_TO_TICKS(ms));
}

static bool joinWifi(const String &ssid, const String &pass, uint32_t timeoutMs, void (*pump)() = nullptr) {
  WiFi.begin(ssid.c_str(), pass.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) pauseMs(250, pump);
  return WiFi.status() == WL_CONNECTED;
}

// Asks the NTP pool for the time. On success sets the system clock, writes the RTC, marks the time as synced.
static bool ntpFetch(uint32_t timeoutMs, void (*pump)() = nullptr) {
  configTzTime(tzString.c_str(), "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  sntp_set_sync_status(SNTP_SYNC_STATUS_RESET);
  uint32_t t0 = millis();
  bool ok = false;
  while (!ok && millis() - t0 < timeoutMs) {
    pauseMs(250, pump);
    ok = sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED;
  }
  if (ok) {
    time_t now = time(nullptr);
    writeRTC(now);
    lastSyncEpoch = now;
    timeSource = TS_SYNCED;
  }
  esp_sntp_stop();
  return ok;
}

// ---------- phone setup portal ----------
static WebServer *portalServer = nullptr;
static DNSServer *portalDns = nullptr;
static String portalOptions;  // <option> list of nearby networks
static String pendSsid, pendPass, pendTz, portalError;
static volatile bool pendReq = false;

static void portalPump() {
  if (portalDns) portalDns->processNextRequest();
  if (portalServer) portalServer->handleClient();
  vTaskDelay(pdMS_TO_TICKS(10));
}

static String htmlEscape(const String &s) {
  String o;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '&') o += "&amp;";
    else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '"') o += "&quot;";
    else if (c == '\'') o += "&#39;";
    else o += c;
  }
  return o;
}

static void scanNetworks() {
  portalOptions = "";
  int n = WiFi.scanNetworks();
  String seen;
  int shown = 0;
  for (int i = 0; i < n && shown < 20; i++) {
    String ssid = WiFi.SSID(i);
    if (!ssid.length() || seen.indexOf("\n" + ssid + "\n") >= 0) continue;
    seen += "\n" + ssid + "\n";
    portalOptions += "<option value=\"" + htmlEscape(ssid) + "\">";
    shown++;
  }
  WiFi.scanDelete();
}

static const char PAGE_HEAD[] PROGMEM =
  "<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
  "<title>Clock setup</title><style>body{font-family:-apple-system,system-ui,sans-serif;margin:0;padding:22px;background:#0b1020;color:#eef}"
  "h1{font-size:23px;margin:4px 0 14px}label{display:block;margin:18px 0 6px;color:#9ab}"
  "input,select,button{width:100%;box-sizing:border-box;padding:13px;font-size:17px;border-radius:9px;border:1px solid #345;background:#16203a;color:#eef}"
  "button{background:#e8a317;color:#000;border:0;margin-top:24px;font-weight:600}"
  ".msg{padding:12px;border-radius:9px;background:#4a2a10;margin-bottom:6px}small{color:#89a}a{color:#e8a317}</style></head><body>";

static String pageForm(const String &msg) {
  String h = FPSTR(PAGE_HEAD);
  h += "<h1>Clock setup</h1>";
  if (msg.length()) h += "<div class=msg>" + htmlEscape(msg) + "</div>";
  h += "<form method=post action=/save><label>Wi-Fi network</label>"
       "<input name=ssid list=nets autocomplete=off autocapitalize=none placeholder='choose or type a name (blank keeps the saved network)'>"
       "<datalist id=nets>" + portalOptions + "</datalist>"
       "<label>Wi-Fi password</label><input name=pass type=password autocomplete=off placeholder='leave empty if open'>"
       "<label>Time zone</label><select name=tz>";
  bool known = false;
  for (int i = 0; i < TZ_COUNT; i++) known |= (tzString == TZ_CHOICES[i].posix);
  if (!known) h += "<option value=\"" + htmlEscape(tzString) + "\" selected>Current setting</option>";
  for (int i = 0; i < TZ_COUNT; i++) {
    h += "<option value=\"";
    h += TZ_CHOICES[i].posix;
    h += "\"";
    if (tzString == TZ_CHOICES[i].posix) h += " selected";
    h += ">";
    h += TZ_CHOICES[i].label;
    h += "</option>";
  }
  h += "</select><label>Clock style</label><select name=hfmt>"
       "<option value=12";
  if (!hour24Format) h += " selected";
  h += ">12-hour (AM/PM)</option><option value=24";
  if (hour24Format) h += " selected";
  h += ">24-hour (military)</option></select>"
       "<label>Units</label><select name=units><option value=F";
  if (useFahrenheit) h += " selected";
  h += ">Fahrenheit and feet</option><option value=C";
  if (!useFahrenheit) h += " selected";
  h += ">Celsius and metres</option></select>"
       "<label>NOAA tide station ID (7 digits)</label>"
       "<input name=tide inputmode=numeric pattern='[0-9]{5,9}' value=\"" + htmlEscape(tideStation) + "\" placeholder='e.g. 9447130'>"
       "<button>Save</button></form>"
       "<p><small>The clock joins your Wi-Fi to read the time and tide predictions from the internet, then turns Wi-Fi off. "
       "Daylight saving is handled by the time zone you pick. Find your station ID at tidesandcurrents.noaa.gov "
       "(pick the nearest station on the map; the 7-digit number is its ID). Leave the network blank to change only the settings.</small></p></body></html>";
  return h;
}

static String pageWorking() {
  String h = FPSTR(PAGE_HEAD);
  h += "<h1>Connecting...</h1><p id=m>Joining your Wi-Fi and reading the time. This can take up to 30 seconds. "
       "Your phone may briefly drop this network; that is normal.</p>"
       "<p><small>If this page stops updating, look at the clock: it shows the result.</small></p>"
       "<script>function poll(){fetch('/status').then(r=>r.json()).then(j=>{"
       "if(j.s==3){document.body.innerHTML='<h1>All set</h1><p>The clock has the right time now. You can close this page.</p>';return}"
       "if(j.s==4){document.getElementById('m').innerHTML='<b>'+j.e+'</b><p><a href=/>Try again</a></p>';return}"
       "setTimeout(poll,1500)}).catch(function(){setTimeout(poll,2000)})}poll()</script></body></html>";
  return h;
}

static void handleRoot() {
  if (setupStatus == 4) setupStatus = 0;
  portalServer->send(200, "text/html", pageForm(""));
}

static void handleStatus() {
  String j = "{\"s\":" + String((int)setupStatus) + ",\"e\":\"" + htmlEscape(portalError) + "\"}";
  portalServer->send(200, "application/json", j);
}

static void handleSave() {
  String ssid = portalServer->arg("ssid"), pass = portalServer->arg("pass"), tz = portalServer->arg("tz");
  String tide = portalServer->arg("tide");
  ssid.trim();
  tide.trim();
  String oldSsid, oldPass;
  bool haveSaved = loadCreds(oldSsid, oldPass);
  if (ssid.length() == 0 && !haveSaved) {
    portalServer->send(200, "text/html", pageForm("Enter your Wi-Fi network name so the clock can get the time."));
    return;
  }
  if (ssid.length() > 32 || pass.length() > 63 || (ssid.length() > 0 && pass.length() > 0 && pass.length() < 8)) {
    portalServer->send(200, "text/html", pageForm("Enter the network name, and a password of 8 to 63 characters (or none for an open network)."));
    return;
  }
  bool tideOk = tide.length() == 0;
  if (!tideOk && tide.length() >= 5 && tide.length() <= 9) {
    tideOk = true;
    for (size_t i = 0; i < tide.length(); i++) tideOk &= isDigit(tide[i]);
  }
  if (!tideOk) {
    portalServer->send(200, "text/html", pageForm("The tide station ID should be 5 to 9 digits, or blank for none."));
    return;
  }
  bool tzOk = (tz == tzString);
  for (int i = 0; i < TZ_COUNT && !tzOk; i++) tzOk = (tz == TZ_CHOICES[i].posix);
  if (!tzOk) tz = tzString;
  hour24Format = portalServer->arg("hfmt") == "24";  // display-only, applies immediately regardless of Wi-Fi result
  saveHour24(hour24Format);
  useFahrenheit = portalServer->arg("units") != "C";
  saveUnits(useFahrenheit);
  if (tide != tideStation) {
    tideStation = tide;
    tideName = "";
    saveTideStation(tide);
    tideStale = true;
  }
  settingsDirty = true;
  if (ssid.length() == 0) {  // settings only: keep the saved Wi-Fi, no reconnect
    applyTz(tz);
    saveTz(tz);
    portalError = "";
    setupStatus = 3;
    portalServer->send(200, "text/html", pageWorking());
    return;
  }
  pendSsid = ssid;
  pendPass = pass;
  pendTz = tz;
  portalError = "";
  setupStatus = 2;
  pendReq = true;
  portalServer->send(200, "text/html", pageWorking());
}

static void handleNotFound() {
  portalServer->sendHeader("Location", "http://192.168.4.1/", true);
  portalServer->send(302, "text/plain", "");
}

static void portalTryPending() {
  pendReq = false;
  String oldTz = tzString;
  applyTz(pendTz);
  bool joined = joinWifi(pendSsid, pendPass, 15000, portalPump);
  bool synced = joined && ntpFetch(15000, portalPump);
  if (synced) {
    saveCreds(pendSsid, pendPass);
    saveTz(pendTz);
    setupStatus = 3;
    Serial.println("setup: connected and time synced");
  } else {
    applyTz(oldTz);
    WiFi.disconnect(false, false);
    portalError = joined ? "Joined the network, but could not reach a time server." : "Could not join that network. Check the name and password.";
    setupStatus = 4;
    Serial.printf("setup: failed (%s)\n", portalError.c_str());
  }
}

static void runSetupPortal() {
  Serial.println("setup: portal starting");
  setupCancel = false;
  setupRequested = false;
  setupStatus = 0;
  pendReq = false;
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(SETUP_AP_SSID);
  vTaskDelay(pdMS_TO_TICKS(300));
  scanNetworks();
  portalDns = new DNSServer();
  portalDns->start(53, "*", WiFi.softAPIP());
  portalServer = new WebServer(80);
  portalServer->on("/", HTTP_GET, handleRoot);
  portalServer->on("/save", HTTP_POST, handleSave);
  portalServer->on("/status", HTTP_GET, handleStatus);
  portalServer->onNotFound(handleNotFound);
  portalServer->begin();
  setupActive = true;

  uint32_t t0 = millis(), doneAt = 0;
  while (!setupCancel && millis() - t0 < 10UL * 60UL * 1000UL) {
    portalPump();
    if (pendReq) portalTryPending();
    if (setupStatus <= 1) setupStatus = WiFi.softAPgetStationNum() > 0 ? 1 : 0;
    if (setupStatus == 3) {
      if (!doneAt) doneAt = millis();
      if (millis() - doneAt > 12000) break;  // leave time for the phone to read the result
    }
  }
  setupActive = false;
  portalServer->stop();
  delete portalServer;
  portalServer = nullptr;
  portalDns->stop();
  delete portalDns;
  portalDns = nullptr;
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  Serial.println("setup: portal closed");
}

// ---------- background task: sync now, then every 6 hours; setup when there is no Wi-Fi saved ----------
static void netTask(void *) {
  static const uint32_t backoff[] = {15000, 30000, 60000, 120000, 300000, 900000};
  int failures = 0;
  bool autoSetupTried = false;
  vTaskDelay(pdMS_TO_TICKS(1500));
  for (;;) {
    String ssid, pass;
    bool haveCreds = loadCreds(ssid, pass);
    if (setupRequested || (!haveCreds && !autoSetupTried)) {
      autoSetupTried = true;
      runSetupPortal();
      failures = 0;
      continue;
    }
    if (!haveCreds) {
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(60000));
      continue;
    }
    WiFi.mode(WIFI_STA);
    bool ok = joinWifi(ssid, pass, 20000) && ntpFetch(20000);
    if (ok) onOnline();  // tide fetch etc. while the connection is up
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    if (ok) {
      Serial.println("ntp: time synced");
      failures = 0;
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(6UL * 3600UL * 1000UL));
    } else {
      failures++;
      Serial.printf("ntp: sync failed (%d), will retry\n", failures);
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(backoff[min(failures - 1, 5)]));
    }
  }
}

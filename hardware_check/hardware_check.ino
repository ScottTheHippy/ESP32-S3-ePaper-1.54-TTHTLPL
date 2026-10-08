// Waveshare ESP32-S3-ePaper-1.54: ESP32-S3-PICO-1 (8MB flash, 8MB quad PSRAM), 200x200
// black/white e-paper (SSD1681 family), PCF85063 RTC, SHTC3 temp/humidity.
// Pins and quirks from Waveshare's own demo repo (waveshareteam/ESP32-S3-ePaper-1.54):
//   - the e-paper power rail on GPIO 6 is ACTIVE LOW (drive it low to power the panel)
//   - GPIO 17 is a battery power latch (hold HIGH to stay on when running from battery)
// Build: FQBN esp32:esp32:esp32s3:CDCOnBoot=cdc,FlashSize=8M
#include <GxEPD2_BW.h>
#include <Fonts/FreeMonoBold9pt7b.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Wire.h>

#define EPD_DC 10
#define EPD_CS 11
#define EPD_SCK 12
#define EPD_MOSI 13
#define EPD_RST 9
#define EPD_BUSY 8
#define EPD_PWR 6     // active low
#define AUDIO_PWR 42  // active low; the RTC/SHTC3 I2C devices only answer with this rail on
#define VBAT_LATCH 17
#define BOOT_BTN 0
#define PWR_BTN 18
#define I2C_SDA 47
#define I2C_SCL 48
#define RTC_ADDR 0x51
#define SHTC3_ADDR 0x70

GxEPD2_BW<GxEPD2_154_D67, GxEPD2_154_D67::HEIGHT> display(GxEPD2_154_D67(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));

static uint8_t bcd(uint8_t v) { return (v & 0x0F) + 10 * (v >> 4); }

bool readSHTC3(float &t, float &rh) {
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

bool readRTC(char *out, size_t n, bool &oscStopped) {
  Wire.beginTransmission(RTC_ADDR); Wire.write(0x04);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(RTC_ADDR, 7) != 7) return false;
  uint8_t s = Wire.read(), mi = Wire.read(), h = Wire.read(), d = Wire.read(), wd = Wire.read(), mo = Wire.read(), y = Wire.read();
  (void)wd;
  oscStopped = s & 0x80;
  snprintf(out, n, "20%02u-%02u-%02u %02u:%02u:%02u", bcd(y), bcd(mo & 0x1F), bcd(d & 0x3F), bcd(h & 0x3F), bcd(mi & 0x7F), bcd(s & 0x7F));
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(2500);
  Serial.println("epaper check: start");
  pinMode(VBAT_LATCH, OUTPUT); digitalWrite(VBAT_LATCH, HIGH);
  pinMode(EPD_PWR, OUTPUT); digitalWrite(EPD_PWR, LOW);  // panel power ON (active low)
  pinMode(AUDIO_PWR, OUTPUT); digitalWrite(AUDIO_PWR, LOW);  // codec/sensor rail ON (active low)
  pinMode(BOOT_BTN, INPUT_PULLUP);
  pinMode(PWR_BTN, INPUT_PULLUP);
  delay(100);  // let the switched rails come up before touching I2C

  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(300000);
  String found;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { char b[8]; snprintf(b, sizeof b, "0x%02X ", a); found += b; }
  }
  Serial.printf("i2c devices: %s\n", found.c_str());

  float t = 0, rh = 0;
  bool okT = readSHTC3(t, rh);
  char rtc[24] = "--";
  bool osc = false;
  bool okR = readRTC(rtc, sizeof rtc, osc);
  Serial.printf("shtc3: %s  %.1f C  %.0f %%RH\n", okT ? "ok" : "FAIL", t, rh);
  Serial.printf("rtc:   %s  %s%s\n", okR ? "ok" : "FAIL", rtc, osc ? "  (oscillator stopped: time invalid)" : "");

  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);
  display.init(115200, true, 2, false);
  display.setRotation(0);
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.drawRect(0, 0, 200, 200, GxEPD_BLACK);
    display.drawRect(3, 3, 194, 194, GxEPD_BLACK);
    display.setTextColor(GxEPD_BLACK);
    display.setFont(&FreeSansBold12pt7b);
    display.setCursor(12, 30);
    display.print("e-Paper 1.54");
    display.setFont(&FreeMonoBold9pt7b);
    display.setCursor(12, 52);
    display.print("200x200  SSD1681");
    // checker + gray-ish dither blocks to judge contrast and ghosting
    for (int y = 0; y < 4; y++)
      for (int x = 0; x < 8; x++)
        if ((x + y) & 1) display.fillRect(12 + x * 22, 64 + y * 14, 22, 14, GxEPD_BLACK);
    display.setCursor(12, 140);
    display.printf("T %.1fC  RH %.0f%%", t, rh);
    display.setCursor(12, 158);
    display.print(okR ? rtc : "RTC not found");
    display.setCursor(12, 176);
    display.print(found.length() ? found.c_str() : "no i2c");
  } while (display.nextPage());
  display.hibernate();
  Serial.println("epaper check: drawn");
}

void loop() {
  static int lastBoot = 1, lastPwr = 1;
  int b = digitalRead(BOOT_BTN), p = digitalRead(PWR_BTN);
  if (b != lastBoot) { lastBoot = b; Serial.printf("BOOT button %s\n", b ? "released" : "pressed"); }
  if (p != lastPwr) { lastPwr = p; Serial.printf("PWR button %s\n", p ? "released" : "pressed"); }
  delay(20);
}

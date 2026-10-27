/*
  SMART BIN  -  ESP32 + HC-SR04 ultrasonic + neon light + 16x2 I2C LCD
  ---------------------------------------------------------------------
  - Measures the distance from the lid to the trash.
  - Detects when something is thrown in (distance suddenly gets shorter).
  - Detects when the bin is FULL (>= 90%): neon ON, LCD warning, phone notification.
  - Detects when the bin is emptied and turns the neon OFF.
  - Shows fill level, a progress bar and counters on the LCD.
  - Saves every event to flash (LittleFS) as CSV, so data survives power loss.
  - Runs a small web server that the dashboard (index.html) reads from.

  WIRING (ESP32 DevKit)
    HC-SR04 VCC  -> 5V (VIN)
    HC-SR04 GND  -> GND
    HC-SR04 TRIG -> GPIO 5
    HC-SR04 ECHO -> GPIO 18  *through a voltage divider* (1k from ECHO to GPIO18,
                                                          2k from GPIO18 to GND)
    Relay module VCC -> 5V (VIN), GND -> GND, IN -> GPIO 26, plus 10k resistor from IN to relay VCC
    Neon (through the relay) -> COM and NO (never NC)
    (MOSFET instead: Gate -> 100-220 ohm -> GPIO 26, Gate -> 10k -> GND, Source -> GND, Drain -> strip (-); set NEON_ACTIVE_HIGH = true)
    LCD I2C  VCC -> 5V (VIN)
    LCD I2C  GND -> GND
    LCD I2C  SDA -> GPIO 21
    LCD I2C  SCL -> GPIO 22

  No extra libraries needed: the LCD driver is built into this file
  (it auto-detects the I2C address 0x27 or 0x3F).

  Arduino IDE: board "ESP32 Dev Module", install the "esp32" board package.
*/

#include <WiFi.h>
#include <WebServer.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <Wire.h>
#include <time.h>

// ===================== SETTINGS - EDIT THESE =====================
const char* WIFI_SSID = "YOUR_WIFI_NAME";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

const long  GMT_OFFSET_SEC = 16200;   // your timezone in seconds (UTC+4:30 = 16200, UTC+4 = 14400)

// Where is this bin? (shown on the dashboard). Leave 0,0 to set it from the website instead.
const char* BIN_NAME = "Smart Bin";   // no quotes inside the name
const float BIN_LAT  = 0.0;
const float BIN_LNG  = 0.0;

// Phone notification when the bin gets full (free, no account): install the "ntfy" app,
// subscribe to the SAME topic name. Pick something unique and hard to guess. "" = disabled.
const char* NTFY_TOPIC = "";          // e.g. "smartbin-mostafa-4821"

#define TRIG_PIN 5
#define ECHO_PIN 18
#define NEON_PIN 26
#define LCD_SDA  21
#define LCD_SCL  22
// Neon driver type.
//   true  = logic-level N-MOSFET (DC neon strip): GPIO HIGH = neon ON.
//   false = active-low relay board (green LED lit = relay ON): GPIO LOW = relay ON.   <-- relay setup (current)
const bool NEON_ACTIVE_HIGH = false;

const float BIN_DEPTH_CM   = 40.0;    // distance from sensor to the EMPTY bin bottom
const float FULL_DIST_CM   = 5.0;     // distance from sensor when the bin is 100% full
const int   FULL_ON_PCT    = 90;      // neon turns ON at this fill level
const int   FULL_OFF_PCT   = 70;      // neon turns OFF below this level (hysteresis)
const float ITEM_DELTA_CM  = 1.0;     // trash got this much closer  -> "item thrown"
const float EMPTY_DELTA_CM = 15.0;    // trash got this much farther -> "bin emptied"
const size_t MAX_LOG_BYTES = 100000;  // old log lines are trimmed after this
// =================================================================

WebServer server(80);
Preferences prefs;

float lastStable = -1;     // last confirmed distance (cm)
float lastDist   = 0;      // latest reading (cm)
int   fillPct    = 0;
bool  isFull     = false;
int   fullVotes  = 0;
uint32_t itemsToday = 0, itemsTotal = 0;
int   savedDay = -1;
uint32_t lastItemT = 0;
unsigned long lastSample = 0, lastWifiTry = 0, lastLcd = 0;

// ---------- tiny I2C LCD driver (HD44780 + PCF8574 backpack, 16x2) ----------
uint8_t lcdAddr = 0;
bool lcdOk = false;
const uint8_t LCD_BL = 0x08, LCD_EN = 0x04, LCD_RS = 0x01;
char lcdCache[2][17] = {"", ""};
char lcdMsg0[17] = "", lcdMsg1[17] = "";
unsigned long lcdMsgUntil = 0;

void lcdExp(uint8_t d) { Wire.beginTransmission(lcdAddr); Wire.write((uint8_t)(d | LCD_BL)); Wire.endTransmission(); }
void lcdPulse(uint8_t d) { lcdExp(d | LCD_EN); delayMicroseconds(2); lcdExp(d & ~LCD_EN); delayMicroseconds(60); }
void lcdNibble(uint8_t d) { lcdExp(d); lcdPulse(d); }
void lcdSend(uint8_t v, uint8_t rs) { lcdNibble((v & 0xF0) | rs); lcdNibble(((v << 4) & 0xF0) | rs); }
void lcdCmd(uint8_t v) { lcdSend(v, 0); }
bool i2cPresent(uint8_t a) { Wire.beginTransmission(a); return Wire.endTransmission() == 0; }

void lcdInit() {
  Wire.begin(LCD_SDA, LCD_SCL);
  if (i2cPresent(0x27)) lcdAddr = 0x27;
  else if (i2cPresent(0x3F)) lcdAddr = 0x3F;
  else { Serial.println("LCD not found (checked 0x27 and 0x3F) - check SDA/SCL wiring."); return; }
  delay(50);
  lcdNibble(0x30); delay(5);
  lcdNibble(0x30); delay(5);
  lcdNibble(0x30); delayMicroseconds(200);
  lcdNibble(0x20); delayMicroseconds(200);   // switch to 4-bit mode
  lcdCmd(0x28);                              // 2 lines, 5x8 font
  lcdCmd(0x0C);                              // display on, no cursor
  lcdCmd(0x06);                              // cursor moves right
  lcdCmd(0x01); delay(3);                    // clear
  lcdOk = true;
  Serial.printf("LCD found at 0x%02X\n", lcdAddr);
}

// writes one 16-char row (pads with spaces); only touches the LCD if the text changed
void lcdLine(uint8_t row, const char* s) {
  if (!lcdOk) return;
  char pad[17]; int i = 0;
  for (; i < 16 && s[i]; i++) pad[i] = s[i];
  for (; i < 16; i++) pad[i] = ' ';
  pad[16] = 0;
  if (memcmp(pad, lcdCache[row], 17) == 0) return;
  memcpy(lcdCache[row], pad, 17);
  lcdCmd(0x80 | (row ? 0x40 : 0x00));
  for (int k = 0; k < 16; k++) lcdSend((uint8_t)pad[k], LCD_RS);
}

// normal screens: fill % + progress bar, alternating with counters (unless full)
void lcdRefresh() {
  if (!lcdOk) return;
  if (millis() < lcdMsgUntil) { lcdLine(0, lcdMsg0); lcdLine(1, lcdMsg1); return; }
  char a[20], b[20];
  bool statsPage = !isFull && ((millis() / 4000) % 2 == 1);
  if (statsPage) {
    snprintf(a, sizeof(a), "Today: %lu", (unsigned long)itemsToday);
    snprintf(b, sizeof(b), "Total: %lu", (unsigned long)itemsTotal);
  } else {
    snprintf(a, sizeof(a), "Fill:%3d%%  %s", fillPct, isFull ? "FULL!" : "OK");
    int n = fillPct * 16 / 100;
    for (int i = 0; i < 16; i++) b[i] = (i < n) ? (char)0xFF : ' ';   // 0xFF = solid block
    b[16] = 0;
  }
  lcdLine(0, a);
  lcdLine(1, b);
}

// show a temporary 2-line message for 'ms' milliseconds
void lcdShowMsg(const char* a, const char* b, unsigned long ms) {
  snprintf(lcdMsg0, sizeof(lcdMsg0), "%s", a);
  snprintf(lcdMsg1, sizeof(lcdMsg1), "%s", b);
  lcdMsgUntil = millis() + ms;
  lcdRefresh();
}

// ---------- helpers ----------
uint32_t nowSec() {
  time_t t = time(nullptr);
  if (t > 1700000000) return (uint32_t)t;   // NTP synced -> real time
  return millis() / 1000;                   // otherwise uptime (dashboard shows "uptime ...")
}

void setNeon(bool on) {
  if (NEON_ACTIVE_HIGH) {
    digitalWrite(NEON_PIN, on ? HIGH : LOW);
  } else {
    // Open-drain: LOW pulls the relay input to GND (relay ON). HIGH only RELEASES the pin, so the relay
    // board's own 5V pull-up switches it fully OFF (a plain 3.3V HIGH can leave a 5V relay board half-on).
    digitalWrite(NEON_PIN, on ? LOW : HIGH);
  }
}

float readOnce() {
  digitalWrite(TRIG_PIN, LOW);  delayMicroseconds(3);
  digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  unsigned long d = pulseIn(ECHO_PIN, HIGH, 26000UL);   // 26 ms timeout (~4.4 m, the sensor's real max)
  if (d == 0) return -1;
  return d * 0.0343f / 2.0f;
}

// 9 readings, drop the extremes, average the middle ones = ignores spikes AND gives sub-cm sensitivity
float readDistance() {
  const int N = 9;
  float v[N]; int n = 0;
  for (int i = 0; i < N; i++) {
    float x = readOnce();
    if (x > 2 && x < 450) v[n++] = x;     // HC-SR04 range: 2 cm .. ~4 m
    delay(15);
  }
  if (n < 5) return -1;
  for (int i = 1; i < n; i++) {
    float k = v[i]; int j = i - 1;
    while (j >= 0 && v[j] > k) { v[j + 1] = v[j]; j--; }
    v[j + 1] = k;
  }
  int a = n / 4, b = n - n / 4;           // skip lowest and highest quarter
  float sum = 0;
  for (int i = a; i < b; i++) sum += v[i];
  return sum / (b - a);
}

int distToPct(float d) {
  float p = (BIN_DEPTH_CM - d) / (BIN_DEPTH_CM - FULL_DIST_CM) * 100.0f;
  if (p < 0) p = 0;
  if (p > 100) p = 100;
  return (int)(p + 0.5f);
}

// ---------- data saving ----------
void trimLog() {
  File f = LittleFS.open("/log.csv", "r");
  if (!f) return;
  size_t sz = f.size();
  if (sz < MAX_LOG_BYTES) { f.close(); return; }
  File out = LittleFS.open("/tmp.csv", "w");
  f.seek(sz / 2);
  f.readStringUntil('\n');                       // skip the partial line
  uint8_t buf[256];
  while (f.available()) { int n = f.read(buf, sizeof(buf)); out.write(buf, n); }
  f.close(); out.close();
  LittleFS.remove("/log.csv");
  LittleFS.rename("/tmp.csv", "/log.csv");
}

// CSV line:  time,event,fill%,distance_cm
void logEvent(const char* type) {
  File f = LittleFS.open("/log.csv", "a");
  if (!f) return;
  f.printf("%lu,%s,%d,%.1f\n", (unsigned long)nowSec(), type, fillPct, lastDist);
  f.close();
  trimLog();
  Serial.printf("[LOG] %s  fill=%d%%  dist=%.1fcm\n", type, fillPct, lastDist);
}

void saveCounters() {
  prefs.putUInt("today", itemsToday);
  prefs.putUInt("total", itemsTotal);
  prefs.putInt("day", savedDay);
  prefs.putUInt("last", lastItemT);
}

void checkDay() {
  struct tm tmv;
  if (!getLocalTime(&tmv, 0)) return;
  if (tmv.tm_yday != savedDay) { savedDay = tmv.tm_yday; itemsToday = 0; saveCounters(); }
}

// ---------- phone notification (ntfy.sh) ----------
void notifyPhone(const char* title, const String& msg, const char* tags, const char* prio) {
  if (NTFY_TOPIC[0] == 0 || WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  http.setConnectTimeout(4000);
  http.setTimeout(4000);
  if (!http.begin(String("http://ntfy.sh/") + NTFY_TOPIC)) return;
  http.addHeader("Title", title);
  http.addHeader("Priority", prio);
  http.addHeader("Tags", tags);
  if (BIN_LAT != 0 || BIN_LNG != 0)           // tapping the notification opens the map
    http.addHeader("Click", String("https://www.google.com/maps?q=") + String(BIN_LAT, 6) + "," + String(BIN_LNG, 6));
  int code = http.POST(msg);
  Serial.printf("[NTFY] HTTP %d\n", code);
  http.end();
}

// ---------- web server ----------
void cors() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server.sendHeader("Cache-Control", "no-store");
}

void handleRoot() {
  if (LittleFS.exists("/index.html")) {
    File f = LittleFS.open("/index.html", "r");
    server.streamFile(f, "text/html");
    f.close();
  } else {
    server.send(200, "text/plain",
      "Smart bin is running. Open the dashboard file (index.html) and enter this device's IP.");
  }
}

void handleStatus() {
  cors();
  checkDay();
  String j = "{\"ok\":true";
  j += ",\"distance\":" + String(lastDist, 1);
  j += ",\"fill\":" + String(fillPct);
  j += ",\"full\":" + String(isFull ? "true" : "false");
  j += ",\"neon\":" + String(isFull ? "true" : "false");
  j += ",\"today\":" + String(itemsToday);
  j += ",\"total\":" + String(itemsTotal);
  j += ",\"t\":" + String((unsigned long)nowSec());
  j += ",\"depth\":" + String(BIN_DEPTH_CM, 0);
  j += ",\"last\":" + String((unsigned long)lastItemT);
  j += ",\"name\":\"" + String(BIN_NAME) + "\"";
  j += ",\"lat\":" + String(BIN_LAT, 6);
  j += ",\"lng\":" + String(BIN_LNG, 6);
  j += "}";
  server.send(200, "application/json", j);
}

void handleLog() {
  cors();
  File f = LittleFS.open("/log.csv", "r");
  if (!f) { server.send(200, "text/csv", ""); return; }
  server.streamFile(f, "text/csv");
  f.close();
}

void handleClear() {
  cors();
  LittleFS.remove("/log.csv");
  itemsToday = 0; itemsTotal = 0; lastItemT = 0;
  saveCounters();
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleNotFound() {
  if (server.method() == HTTP_OPTIONS) { cors(); server.send(204); return; }
  server.send(404, "text/plain", "Not found");
}

// ---------- setup ----------
void connectWifi() {
  lcdLine(0, "Smart Bin");
  lcdLine(1, "WiFi connecting");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to WiFi");
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) { delay(400); Serial.print("."); }
  if (WiFi.status() == WL_CONNECTED) {
    String ip = WiFi.localIP().toString();
    Serial.printf("\nConnected. Open the dashboard and use IP: %s\n", ip.c_str());
    configTime(GMT_OFFSET_SEC, 0, "pool.ntp.org", "time.google.com");
    if (MDNS.begin("smartbin")) Serial.println("Also reachable at: smartbin.local");
    lcdShowMsg("Dashboard IP:", ip.c_str(), 4000);
  } else {
    Serial.println("\nWiFi failed - bin still works and saves data, will retry.");
    lcdShowMsg("WiFi failed", "Running offline", 3000);
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(NEON_PIN, NEON_ACTIVE_HIGH ? OUTPUT : OUTPUT_OPEN_DRAIN);
  setNeon(false);                       // neon/relay OFF from the very start

  lcdInit();
  lcdLine(0, "Smart Bin");
  lcdLine(1, "Starting...");

  if (!LittleFS.begin(true)) Serial.println("LittleFS mount failed");

  prefs.begin("bin", false);
  itemsToday = prefs.getUInt("today", 0);
  itemsTotal = prefs.getUInt("total", 0);
  savedDay   = prefs.getInt("day", -1);
  lastItemT  = prefs.getUInt("last", 0);

  connectWifi();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/log.csv", HTTP_GET, handleLog);
  server.on("/api/clear", HTTP_POST, handleClear);
  server.onNotFound(handleNotFound);
  server.begin();

  float d = readDistance();
  if (d > 0) { lastStable = min(d, BIN_DEPTH_CM); lastDist = lastStable; fillPct = distToPct(lastStable); }
  logEvent("boot");
}

// ---------- main loop ----------
void loop() {
  server.handleClient();

  if (millis() - lastLcd > 500) { lastLcd = millis(); lcdRefresh(); }

  if (WiFi.status() != WL_CONNECTED && millis() - lastWifiTry > 15000) {
    lastWifiTry = millis();
    WiFi.reconnect();
  }

  if (millis() - lastSample < 150) return;
  lastSample = millis();

  float d = readDistance();
  if (d < 0) return;                       // sensor error -> skip this round
  if (d > BIN_DEPTH_CM) d = BIN_DEPTH_CM;
  lastDist = d;

  if (lastStable < 0) lastStable = d;

  float diff = lastStable - d;             // positive = trash got closer to the sensor

  if (diff >= ITEM_DELTA_CM) {
    // Something got closer. Wait and measure again so a falling item passing the sensor isn't counted twice.
    delay(400);
    float d2 = readDistance();
    if (d2 > 0 && (lastStable - d2) >= ITEM_DELTA_CM) {
      lastStable = min(d2, BIN_DEPTH_CM);
      lastDist = lastStable;
      fillPct = distToPct(lastStable);
      checkDay();
      itemsToday++; itemsTotal++;
      lastItemT = nowSec();
      saveCounters();
      logEvent("item");
      char b[17];
      snprintf(b, sizeof(b), "Today: %lu", (unsigned long)itemsToday);
      lcdShowMsg("Item thrown in!", b, 1200);
    }
  } else if (-diff >= EMPTY_DELTA_CM) {
    delay(400);
    float d2 = readDistance();
    if (d2 > 0 && (d2 - lastStable) >= EMPTY_DELTA_CM) {
      lastStable = min(d2, BIN_DEPTH_CM);
      lastDist = lastStable;
      fillPct = distToPct(lastStable);
      isFull = false; fullVotes = 0;
      setNeon(false);
      logEvent("emptied");
      lcdShowMsg("Bin emptied", "Thank you!", 2500);
    }
  } else {
    lastStable = d;                        // small drift (trash settling) - just follow it
    fillPct = distToPct(lastStable);
  }

  // ----- FULL detection with debounce + hysteresis -----
  if (!isFull) {
    if (fillPct >= FULL_ON_PCT) fullVotes++; else fullVotes = 0;
    if (fullVotes >= 3) {                  // 3 readings in a row
      isFull = true;
      setNeon(true);
      logEvent("full");
      lcdShowMsg("!! BIN FULL !!", "Please empty me", 3000);
      notifyPhone("Smart Bin is FULL",
                  String(BIN_NAME) + " is " + fillPct + "% full. Please empty it.",
                  "wastebasket,warning", "high");
    }
  } else if (fillPct < FULL_OFF_PCT) {
    isFull = false; fullVotes = 0;
    setNeon(false);
  }
}

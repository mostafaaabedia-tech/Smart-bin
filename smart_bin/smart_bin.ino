/*
  SMART BIN  -  ESP32 + HC-SR04 ultrasonic + neon light
  ------------------------------------------------------
  - Measures the distance from the lid to the trash.
  - Detects when something is thrown in (distance suddenly gets shorter).
  - Detects when the bin is FULL (>= 90%) and turns the neon ON.
  - Detects when the bin is emptied and turns the neon OFF.
  - Saves every event to flash (LittleFS) as CSV, so data survives power loss.
  - Runs a small web server that the dashboard (index.html) reads from.

  WIRING (ESP32 DevKit)
    HC-SR04 VCC  -> 5V (VIN)
    HC-SR04 GND  -> GND
    HC-SR04 TRIG -> GPIO 5
    HC-SR04 ECHO -> GPIO 18  *through a voltage divider* (1k from ECHO to GPIO18,
                                                          2k from GPIO18 to GND)
    Neon driver  -> GPIO 26  (relay module / MOSFET, NOT the neon directly)

  Arduino IDE: board "ESP32 Dev Module", install the "esp32" board package.
  Libraries used are all built in (WiFi, WebServer, LittleFS, Preferences, ESPmDNS).
*/

#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <time.h>

// ===================== SETTINGS - EDIT THESE =====================
const char* WIFI_SSID = "YOUR_WIFI_NAME";
const char* WIFI_PASS = "YOUR_WIFI_PASSWORD";

const long  GMT_OFFSET_SEC = 16200;   // your timezone in seconds (UTC+4:30 = 16200, UTC+4 = 14400)

#define TRIG_PIN 5
#define ECHO_PIN 18
#define NEON_PIN 26
const bool NEON_ACTIVE_HIGH = true;   // set false if your relay turns ON with LOW

const float BIN_DEPTH_CM   = 40.0;    // distance from sensor to the EMPTY bin bottom
const float FULL_DIST_CM   = 5.0;     // distance from sensor when the bin is 100% full
const int   FULL_ON_PCT    = 90;      // neon turns ON at this fill level
const int   FULL_OFF_PCT   = 70;      // neon turns OFF below this level (hysteresis)
const float ITEM_DELTA_CM  = 2.0;     // trash got this much closer  -> "item thrown"
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
unsigned long lastSample = 0, lastWifiTry = 0;

// ---------- helpers ----------
uint32_t nowSec() {
  time_t t = time(nullptr);
  if (t > 1700000000) return (uint32_t)t;   // NTP synced -> real time
  return millis() / 1000;                   // otherwise uptime (dashboard shows "uptime ...")
}

void setNeon(bool on) {
  digitalWrite(NEON_PIN, (on == NEON_ACTIVE_HIGH) ? HIGH : LOW);
}

float readOnce() {
  digitalWrite(TRIG_PIN, LOW);  delayMicroseconds(3);
  digitalWrite(TRIG_PIN, HIGH); delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  unsigned long d = pulseIn(ECHO_PIN, HIGH, 30000UL);   // 30 ms timeout (~5 m)
  if (d == 0) return -1;
  return d * 0.0343f / 2.0f;
}

// median of 5 readings = ignores random spikes
float readDistance() {
  float v[5]; int n = 0;
  for (int i = 0; i < 5; i++) {
    float x = readOnce();
    if (x > 1 && x < 400) v[n++] = x;
    delay(30);
  }
  if (n < 3) return -1;
  for (int i = 1; i < n; i++) {
    float k = v[i]; int j = i - 1;
    while (j >= 0 && v[j] > k) { v[j + 1] = v[j]; j--; }
    v[j + 1] = k;
  }
  return v[n / 2];
}

int distToPct(float d) {
  float p = (BIN_DEPTH_CM - d) / (BIN_DEPTH_CM - FULL_DIST_CM) * 100.0f;
  if (p < 0) p = 0; if (p > 100) p = 100;
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
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting to WiFi");
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) { delay(400); Serial.print("."); }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nConnected. Open the dashboard and use IP: %s\n", WiFi.localIP().toString().c_str());
    configTime(GMT_OFFSET_SEC, 0, "pool.ntp.org", "time.google.com");
    if (MDNS.begin("smartbin")) Serial.println("Also reachable at: smartbin.local");
  } else {
    Serial.println("\nWiFi failed - bin still works and saves data, will retry.");
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(NEON_PIN, OUTPUT);
  setNeon(false);

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

  if (WiFi.status() != WL_CONNECTED && millis() - lastWifiTry > 15000) {
    lastWifiTry = millis();
    WiFi.reconnect();
  }

  if (millis() - lastSample < 300) return;
  lastSample = millis();

  float d = readDistance();
  if (d < 0) return;                       // sensor error -> skip this round
  if (d > BIN_DEPTH_CM) d = BIN_DEPTH_CM;
  lastDist = d;

  if (lastStable < 0) lastStable = d;

  float diff = lastStable - d;             // positive = trash got closer to the sensor

  if (diff >= ITEM_DELTA_CM) {
    // Something got closer. Wait and measure again so a falling item passing the sensor isn't counted twice.
    delay(600);
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
    }
  } else if (-diff >= EMPTY_DELTA_CM) {
    delay(600);
    float d2 = readDistance();
    if (d2 > 0 && (d2 - lastStable) >= EMPTY_DELTA_CM) {
      lastStable = min(d2, BIN_DEPTH_CM);
      lastDist = lastStable;
      fillPct = distToPct(lastStable);
      isFull = false; fullVotes = 0;
      setNeon(false);
      logEvent("emptied");
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
    }
  } else if (fillPct < FULL_OFF_PCT) {
    isFull = false; fullVotes = 0;
    setNeon(false);
  }
}

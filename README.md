# Smart Bin (ESP32 + ultrasonic + neon + LCD)

## Files
- `smart_bin/smart_bin.ino` – ESP32 firmware (Arduino IDE, no extra libraries)
- `index.html` – dashboard (shows demo data if no ESP32 is found)

## Wiring
| Part | ESP32 |
|---|---|
| HC-SR04 VCC | 5V / VIN |
| HC-SR04 GND | GND |
| HC-SR04 TRIG | GPIO 5 |
| HC-SR04 ECHO | GPIO 18 **via divider** (1k in series, 2k to GND) – ECHO is 5V, ESP32 pins are 3.3V |
| Neon driver (relay or MOSFET) | GPIO 26 |
| LCD 16x2 I2C VCC | 5V / VIN |
| LCD 16x2 I2C GND | GND |
| LCD 16x2 I2C SDA | GPIO 21 |
| LCD 16x2 I2C SCL | GPIO 22 |

Mount the sensor on the **lid, pointing down**. Don't drive the neon straight from a GPIO.
If the neon is mains powered, use a proper relay module and keep the mains side insulated.

The LCD address (0x27 or 0x3F) is detected automatically. If the screen stays blank, turn the small blue
contrast screw on the back of the I2C module.

## Setup
1. Open `smart_bin.ino` and set `WIFI_SSID`, `WIFI_PASS` and `BIN_DEPTH_CM` (sensor to empty bottom).
2. Optional: set `BIN_NAME`, `BIN_LAT`, `BIN_LNG` (bin location) and `NTFY_TOPIC` (phone alerts, see below).
3. Upload to the ESP32. The LCD shows the dashboard IP at start (also in the Serial Monitor, 115200).
4. Open `index.html`, type the ESP32 IP (or `smartbin.local`) and press Connect.

Optional: upload `index.html` to the ESP32's LittleFS to open the dashboard at `http://<ESP32-IP>/`.

## What it does
- Reads distance every 0.3 s (median of 5 readings).
- Distance drops by >= 2 cm -> **item thrown**, counted and saved.
- Fill >= 90% for 3 readings -> **FULL**: neon ON, LCD warning, phone notification.
  Neon turns OFF when emptied or below 70%.
- Distance jumps up >= 15 cm -> **bin emptied**.
- LCD: fill % and a progress bar, alternating with today/total counters; short messages for each event.
- Every event is saved to flash as `time,event,fill,distance` (`/log.csv`) and survives power loss.

## Notifications when the bin is full
- **Dashboard (page open):** red banner, beep, vibration and flashing tab title. Optional browser
  notification (browsers only allow these on `https` pages or localhost).
- **Phone, even when the page is closed:** pick a unique topic name, put it in `NTFY_TOPIC`, install the free
  **ntfy** app and subscribe to the same topic. The ESP32 sends the alert straight to your phone.
  If `BIN_LAT/BIN_LNG` are set, tapping the notification opens the bin's location on the map.

## Bin location
The dashboard has a location card with a map. Type the coordinates, or press "Use my location" while standing
next to the bin, then Save (stored in your browser). Or set `BIN_NAME / BIN_LAT / BIN_LNG` in the firmware.

## Note about GitHub Pages
A page served over `https` cannot read data from the ESP32's plain `http` address (browser rule), so the online
version shows demo data. For real data open `index.html` from your computer, or serve it from the ESP32.

## API
- `GET /api/status` – live JSON
- `GET /log.csv` – saved events
- `POST /api/clear` – wipe data

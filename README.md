# Smart Bin (ESP32 + ultrasonic + neon)

## Files
- `smart_bin/smart_bin.ino` – ESP32 firmware (Arduino IDE)
- `index.html` – dashboard (works on its own in demo mode)

## Wiring
| Part | ESP32 |
|---|---|
| HC-SR04 VCC | 5V / VIN |
| HC-SR04 GND | GND |
| HC-SR04 TRIG | GPIO 5 |
| HC-SR04 ECHO | GPIO 18 **via divider** (1k in series, 2k to GND) – ECHO is 5V, ESP32 pins are 3.3V |
| Neon driver (relay or MOSFET) | GPIO 26 |

Mount the sensor on the **lid, pointing down**. Don't drive the neon straight from a GPIO.
If the neon is mains powered, use a proper relay module and keep the mains side insulated.

## Setup
1. Open `smart_bin.ino`, set `WIFI_SSID`, `WIFI_PASS`, and measure the bin: set `BIN_DEPTH_CM` (sensor to empty bottom).
2. Upload to the ESP32. Open Serial Monitor (115200) to see the IP address.
3. Open `index.html` in a browser, type the ESP32 IP (or `smartbin.local`) and press Connect.
   (Without an ESP32 it shows demo data.)

Optional: upload `index.html` to the ESP32's LittleFS to open the dashboard directly at `http://<ESP32-IP>/`.

## What it does
- Reads distance every 0.3 s (median of 5 readings).
- Distance drops by >= 2 cm -> **item thrown**, counted and saved.
- Fill >= 90% for 3 readings -> **FULL**, neon ON. Neon turns OFF when emptied or below 70%.
- Distance jumps up >= 15 cm -> **bin emptied**.
- Every event is saved to flash as `time,event,fill,distance` (`/log.csv`) and survives power loss.

## API
- `GET /api/status` – live JSON
- `GET /log.csv` – saved events
- `POST /api/clear` – wipe data

<div align="center">

# 🐣 EmbryoGuard Pro

### IoT-Based Smart Embryo Incubator System

![ESP32](https://img.shields.io/badge/ESP32-FreeRTOS-E7352C?logo=espressif&logoColor=white)
![Python](https://img.shields.io/badge/Python-Flask-3776AB?logo=python&logoColor=white)
![Socket.IO](https://img.shields.io/badge/Socket.IO-Real--time-010101?logo=socketdotio&logoColor=white)
![SQLite](https://img.shields.io/badge/SQLite-Logging-003B57?logo=sqlite&logoColor=white)
![Arduino](https://img.shields.io/badge/Arduino-C%2B%2B-00979D?logo=arduino&logoColor=white)

*CSE272 – Embedded Systems · Faculty of Computer Science and Engineering · Galala University · Spring 2025/26*

</div>

---

## 📖 Overview

Embryo incubation is a critical step in poultry farming, and hatch rates depend on tight environmental control. Traditional incubators often lack real-time monitoring, remote access, and intelligent automation.

**EmbryoGuard Pro** addresses this with a dual-node IoT system built on a single dual-core **ESP32** running **FreeRTOS**, paired with a **Flask + Socket.IO** web dashboard. It regulates temperature with a hysteresis controller (±0.5 °C), monitors air quality and light, turns the eggs automatically, and lets operators watch and control everything live from any device on the local network.

<div align="center">
  <img src="Photos%26GUI/Project%20Photos/photo1.png" alt="EmbryoGuard Pro incubator" width="600">
</div>

## ✨ Features

- **Dual-node architecture on one ESP32:** Node 1 (Core 1) handles climate control; Node 2 (Core 0) handles environmental monitoring.
- **Hysteresis-based temperature control** with a ±0.5 °C band to prevent relay chattering.
- **Multi-species profiles:** Chicken, Duck, Quail and Goose, with dynamic thresholds that sync between dashboard and firmware.
- **Automatic egg turning** with an SG90 servo (4-state sweep, attaches and detaches to avoid buzzing).
- **Real-time dashboard:** circular gauges, live Chart.js graphs, incubation day tracker, and multi-level alerts.
- **AUTO / MANUAL modes** with safety overrides.
- **Mobile access via QR code:** scan and monitor from your phone.
- **Persistent logging** of all sensor data in SQLite.
- **Safety first:** danger-zone override, fail-safe shutdown, hardware watchdog, and manual-mode timeout.

## 🖥️ Dashboard Preview

<p align="center">
  <img src="Photos%26GUI/Dashboard%20GUI/screenshot1.png" alt="Dashboard: gauges, egg type selector and actuator controls" width="800">
</p>
<p align="center">
  <img src="Photos%26GUI/Dashboard%20GUI/screenshot2.png" alt="Dashboard: live temperature and humidity charts" width="800">
</p>

## 🏗️ System Architecture

```
┌──────────────────────────── ESP32 (LILYGO TTGO T-Display) ────────────────────────────┐
│  Core 1 · Node 1: Climate Controller        Core 0 · Node 2: Sensor Monitor           │
│  DHT22 · Fan/Lamp relays · SG90 servo       DHT22 · MQ-135 · LDR                      │
└───────────────┬───────────────────────────────────────────────┬───────────────────────┘
                │ HTTP POST (every 3 s)  ▲ GET /api/commands    │ HTTP POST (every 3 s)
                ▼                        │ (polling)            ▼
        ┌────────────────────── Flask + Flask-SocketIO server (port 8080) ──────────────┐
        │  SQLite logging · Alert engine · Command queue · REST API                     │
        └───────────────────────────────┬───────────────────────────────────────────────┘
                                        │ WebSocket (Socket.IO)
                                        ▼
                         Browser dashboard (desktop / mobile via QR)
```

| Layer | Description |
|---|---|
| **Hardware** | Sensors (DHT22, MQ-135, LDR) and actuators (relay-controlled fan and lamp, SG90 servo) |
| **Firmware** | ESP32 running FreeRTOS with two main tasks pinned to separate cores |
| **Application** | Flask server with SocketIO, SQLite database, and an HTML/CSS/JS dashboard |

## 🔧 Hardware

### Components

- LILYGO TTGO T-Display ESP32 (dual-core Xtensa LX6 @ 240 MHz, Wi-Fi, Bluetooth)
- 2 × DHT22 temperature/humidity sensors
- MQ-135 gas/ammonia sensor
- LDR (photoresistor) with 10 kΩ voltage divider
- 2-channel relay module (fan and heating lamp)
- SG90 servo motor (egg turner)
- Breadboards, jumper wires, and power adapters

### Pin Mapping

| Node | GPIO | Direction | Component |
|---|---|---|---|
| Node 1 | 21 | Input | DHT22 data |
| Node 1 | 27 | Output | Relay 1: Fan (active HIGH) |
| Node 1 | 26 | Output | Relay 2: Lamp (active HIGH) |
| Node 1 | 13 | Output | SG90 servo PWM (50 Hz, 500–2400 µs) |
| Node 2 | 22 | Input | DHT22 data |
| Node 2 | 32 | Analog in | MQ-135 AOUT |
| Node 2 | 17 | Digital in | MQ-135 DOUT |
| Node 2 | 33 | Analog in | LDR (voltage divider) |

> **Power:** use 3.3 V for the sensors (DHT22, MQ-135, LDR) and 5 V for the relay module (VCC and JD-VCC) and the servo. All components share a common GND.

### 📷 Project Photos

<table>
  <tr>
    <td><img src="Photos%26GUI/Project%20Photos/photo2.png" alt="Project photo 2"></td>
    <td><img src="Photos%26GUI/Project%20Photos/photo3.png" alt="Project photo 3"></td>
    <td><img src="Photos%26GUI/Project%20Photos/photo4.png" alt="Project photo 4"></td>
  </tr>
  <tr>
    <td><img src="Photos%26GUI/Project%20Photos/photo5.png" alt="Project photo 5"></td>
    <td><img src="Photos%26GUI/Project%20Photos/photo6.png" alt="Project photo 6"></td>
    <td><img src="Photos%26GUI/Project%20Photos/photo7.png" alt="Project photo 7"></td>
  </tr>
</table>

## 🌡️ Control Logic

The auto-regulation system uses a ±0.5 °C hysteresis band around the target temperature. Example for the default chicken profile (target = 37.5 °C):

| Action | Condition |
|---|---|
| Lamp ON (heating) | Temperature drops to target − 0.5 °C (37.0 °C) |
| Lamp OFF | Temperature rises to target + 0.5 °C (38.0 °C) |
| Fan ON (cooling) | Temperature rises to target + 0.5 °C (38.0 °C) |
| Fan OFF | Temperature drops to target − 0.5 °C (37.0 °C) |
| 🚨 Emergency | Fan forced ON at ≥ 39.5 °C; lamp forced ON at ≤ 35.0 °C |

### 🥚 Multi-Species Profiles

| Species | Target Temp (°C) | Cold Warning Below (°C) | Hot Warning Above (°C) | Incubation Days |
|---|---|---|---|---|
| Chicken 🐔 | 37.5 | 36.5 | 38.5 | 21 |
| Duck 🦆 | 37.7 | 37.0 | 39.0 | 28 |
| Quail 🐦 | 37.8 | 37.5 | 39.5 | 18 |
| Goose 🦢 | 37.6 | 36.8 | 38.8 | 25 |

Selecting a species updates the target temperature, the warning thresholds, and the incubation length. The lamp and fan always switch at **target ∓ 0.5 °C** (see the table above), while the warning columns drive the dashboard's cold/hot warning indicators.

## 🛡️ Safety & Alert System

| Feature | Behavior |
|---|---|
| **Danger override** | Extreme temperatures (≥ 39.5 °C or ≤ 35.0 °C) trigger emergency actuator states and bypass manual commands |
| **Fail-safe shutdown** | Total sensor failure (NaN readings) turns all heating and cooling actuators OFF |
| **Watchdog timer** | 10-second hardware WDT (`esp_task_wdt`) resets the ESP32 on firmware hangs |
| **Timeout revert** | Manual mode auto-reverts to AUTO after 15 minutes of inactivity |
| **Node offline detection** | 15-second heartbeat timeout |

**Alert thresholds**

| Metric | ⚠️ Warning | 🚨 Danger |
|---|---|---|
| Temperature | Above `temp_hot` / below `temp_cold` | ≥ 39.5 °C / ≤ 35.0 °C |
| Humidity | > 65 % / < 45 % | ≥ 75 % (mold risk) / ≤ 38 % (desiccation risk) |
| Ammonia (MQ-135) | ADC ≥ 800 | ADC ≥ 1200 or DOUT = LOW |
| Light (LDR) | ≥ 60 % | ≥ 85 % (lid open?) |

## 💻 Software Stack

**Firmware:** C/C++ (Arduino framework), FreeRTOS, `DHT.h`, `ESP32Servo.h`, `WiFi.h`, `HTTPClient.h`, `ArduinoJson.h`

**Dashboard:** Python 3, Flask 3.0.0, Flask-SocketIO 5.3.6, SQLite3, HTML5/CSS3, vanilla JavaScript, Chart.js, qrcodejs

### FreeRTOS Tasks

| Task | Core | Stack | Priority | Function |
|---|---|---|---|---|
| `node1Task` | 1 | 10240 B | 1 | Climate control, sensor read, HTTP POST, command poll |
| `node2Task` | 0 | 10240 B | 1 | Environmental monitoring, sensor read, HTTP POST |
| `servoTask` | 1 | 4096 B | 2 | Smooth servo sweeping with attach/detach management |

Two mutexes protect shared resources: `http_mutex` serializes HTTP client access across cores, and `serial_mutex` prevents interleaved serial output.

## 🚀 Getting Started

### 1. Run the dashboard server

```bash
git clone https://github.com/smsosa1000/EmbryoGuard-Pro.git
cd EmbryoGuard-Pro/dashboard

python -m venv venv
source venv/bin/activate

pip install -r requirements.txt

python app.py
```

On Windows, activate the virtual environment with `venv\Scripts\activate` instead. The virtual environment is optional.

The dashboard is served on **port 8080**. Open `http://<your-computer-ip>:8080` in a browser, or scan the **Connect Mobile** QR code on the dashboard.

### 2. Flash the firmware

1. Install the **Arduino IDE** with the **ESP32 board package**.
2. Install the libraries: `DHT sensor library` (Adafruit, plus its `Adafruit Unified Sensor` dependency), `ESP32Servo`, and `ArduinoJson` **v7** (the firmware uses `JsonDocument`).
3. Open `incubator_merged/incubator_merged.ino` and set your Wi-Fi credentials and the dashboard server address:
   ```cpp
   const char* WIFI_SSID     = "YOUR_WIFI_NAME";
   const char* WIFI_PASSWORD = "YOUR_WIFI_PASSWORD";
   const char* DASHBOARD_URL = "http://<server-ip>:8080";
   ```
4. Select the **TTGO T-Display** (ESP32) board and upload.

The ESP32 and the computer running the server must be on the **same network**.

## 📁 Project Structure

```
EmbryoGuard-Pro/
├── dashboard/                  # Flask + Socket.IO web application
│   ├── static/
│   │   ├── css/style.css       # Dashboard styling (glassmorphism dark theme)
│   │   ├── images/             # Background pattern
│   │   └── js/dashboard.js     # Real-time UI logic (Socket.IO, Chart.js)
│   ├── templates/index.html    # Main dashboard page
│   ├── app.py                  # Server, REST API, alert engine, SQLite logging
│   ├── incubator.db            # SQLite database
│   └── requirements.txt        # Python dependencies
├── incubator_merged/
│   └── incubator_merged.ino    # ESP32 firmware (Node 1 + Node 2, FreeRTOS)
├── Documents/                  # Project report and presentation (PDF)
└── Photos&GUI/                 # Dashboard screenshots and hardware photos
```

## 📡 API Reference

### REST Endpoints

| Endpoint | Method | Description |
|---|---|---|
| `/` | GET | Serve the main dashboard page |
| `/api/data` | POST | Receive sensor data from ESP32 nodes |
| `/api/commands/<nid>` | GET | ESP32 polls for queued commands |
| `/api/state` | GET | Fetch the full system state on page load |
| `/api/settings` | POST | Update automatic-mode threshold settings |
| `/api/egg-type` | POST | Change egg type and apply the profile thresholds |
| `/api/server-info` | GET | Return server IP/port for QR code generation |

### WebSocket Events

`sensor_update` (server → client) · `control` (client → server) · `command_ack` (server → client) · `egg_type_update` · `settings_update`

### Example Payload (Node 1)

```json
{
  "node": 1,
  "temperature": 37.5,
  "humidity": 55.0,
  "fan": false,
  "lamp": "OFF",
  "servo_angle": 90,
  "servo_turns": 5,
  "incubation_day": 3,
  "sensor_ok": true,
  "cycle": 42,
  "mode": "AUTO",
  "egg_type": "chicken"
}
```

Supported commands: `mode` (AUTO/MANUAL), `fan` (true/false), `lamp` (OFF/FULL), `servo` (turn), `egg_type` (chicken/duck/quail/goose), `sweep` (ms), `dwell` (ms).

## 🗄️ Database

All readings are logged to SQLite (`incubator.db`) in the `sensor_log` table:

| Column | Type | Description |
|---|---|---|
| `id` | INTEGER PK | Auto-incrementing record ID |
| `ts` | TEXT | ISO 8601 timestamp |
| `node` | INTEGER | Node ID (1 or 2) |
| `temperature` | REAL | Temperature in °C |
| `humidity` | REAL | Relative humidity % |
| `extra` | TEXT (JSON) | Additional data (fan, lamp, servo, gas, light, etc.) |

## 🧪 Testing & Validation

- **Hardware:** relay self-test on boot, servo init check, 12-bit ADC with 11 dB attenuation, 5-sample averaging for MQ-135 and LDR.
- **Communication:** 2500 ms HTTP timeout, Wi-Fi retry (20 × 400 ms) with standalone fallback, clear-on-read command queue.
- **Safety:** danger-zone override, 15-minute manual revert, sensor-failure fail-safe, and watchdog reset were all verified.

## 🔮 Future Work

- ☁️ Cloud integration (AWS IoT / Firebase) for remote monitoring beyond the local network
- 🤖 ML-based predictive analytics for hatch-rate optimization
- 📷 Camera module for egg candling and embryo tracking
- 📱 GSM/SMS alerts when Wi-Fi is unavailable
- 🔄 OTA firmware updates from the dashboard
- 🏭 Multi-incubator support with a centralized dashboard
- 🔋 Battery backup with UPS monitoring
- 📊 Historical data export (CSV/PDF)

## 👥 Team

**Supervised by Dr. Safa Elaskary**, Galala University

| Member | ID | Program | Contributions |
|---|---|---|---|
| **Khadiga Osama** | 223102197 | AIE | Node 2 firmware, pin configuration and safety verification, software code, dashboard frontend, presentation and report |
| **Mirna Walid** | 223104705 | AIS | Dashboard backend, SQLite schema and logging, alert evaluation engine, hardware sourcing |
| **Samaa Abdelhamid** | 223101629 | CE | System architecture lead, Node 1 firmware, team coordination and final report, hardware assembly and wiring, physical incubator enclosure |
| **Omar Khalifa** | 223101614 | CE | ESP32 node wiring, power distribution, enclosure layout, hardware self-tests and documentation |
| **Salma Abdelhamid** | 223102401 | AIS | Browser interface, glassmorphism dark theme and CSS, animated actuator controls, report co-author, presentation slides |

## 📚 References

1. Espressif Systems, *ESP32 Technical Reference Manual*, 2024
2. FreeRTOS, *Real-Time Kernel Documentation*, freertos.org
3. Aosong Electronics, *DHT22 Datasheet*, 2020
4. Zhengzhou Winsen Electronics, *MQ-135 Gas Sensor Technical Data*, 2019
5. Flask Documentation, flask.palletsprojects.com
6. Socket.IO Documentation, socket.io/docs
7. ArduinoJson v7 Documentation, arduinojson.org
8. Chart.js, chartjs.org

## 📄 License

This project was developed for academic purposes as part of CSE272 at Galala University.
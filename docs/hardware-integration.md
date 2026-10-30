# Hardware Integration Guide

## Smart Water Management & Pump Health Monitoring System

---

## 1. Bill of Materials

| # | Component | Model / Spec | Qty | Notes |
|---|---|---|---|---|
| 1 | Microcontroller | ESP32 DevKit (38-pin or 30-pin) | 1 | ESP32-WROOM-32 recommended |
| 2 | Ultrasonic distance sensor | HC-SR04 (3.3–5 V compatible) | 1 | 5 V VCC, logic-level-shift ECHO if needed |
| 3 | Water flow sensor | YF-S201 (1–30 L/min) | 1 | G½ thread, food-safe option available |
| 4 | Pressure sensor | 0–5 bar, 0.5–4.5 V analog output | 1 | G¼ or G½ thread to match pipe |
| 5 | Current sensor | ACS712-20A module | 1 | Board includes decoupling capacitor |
| 6 | Relay module | 5 V single-channel, active HIGH | 1 | Must handle pump rated voltage/current |
| 7 | Power supply (ESP32) | 5 V USB or regulated 3.3 V, ≥500 mA | 1 | Stable supply is critical for ADC accuracy |
| 8 | Power supply (pump) | Per pump specification | 1 | Separate from ESP32 supply |
| 9 | Logic level shifter | 3.3 V ↔ 5 V bidirectional (if HC-SR04 is 5 V logic) | 1 | Optional if HC-SR04 ECHO is 5 V |
| 10 | Breadboard or PCB | 170-point mini or custom PCB | 1 | |
| 11 | Jumper wires | Dupont M-M, M-F | ≥30 | |
| 12 | Waterproof enclosure | IP65 or better | 1 | For outdoor / wet installation |
| 13 | USB cable | Micro-USB or USB-C (per board) | 1 | For flashing and serial debug |

**Optional:**
- DS3231 RTC module – for accurate ISO 8601 timestamps without NTP
- 0.96" OLED I²C display – local status display
- Terminal blocks – for permanent wiring

---

## 2. Pin Assignments

| Signal | ESP32 GPIO | Direction | Notes |
|---|---|---|---|
| `TRIG_PIN` | GPIO 5 | Output | HC-SR04 trigger (10 µs pulse) |
| `ECHO_PIN` | GPIO 18 | Input | HC-SR04 echo return; 5 V → 3.3 V shift if sensor is 5 V |
| `FLOW_SENSOR_PIN` | GPIO 19 | Input + Interrupt | YF-S201 pulse output; use `INPUT_PULLUP` |
| `PRESSURE_PIN` | GPIO 34 | ADC Input | ADC1 channel 6; input-only pin |
| `CURRENT_PIN` | GPIO 35 | ADC Input | ADC1 channel 7; input-only pin |
| `PUMP_RELAY_PIN` | GPIO 2 | Output | Active HIGH; drive relay IN pin |
| GND | GND | — | Common ground for all sensors |
| 3V3 | 3.3 V | Power out | ACS712 VCC (module has own regulator) |
| VIN / 5V | 5 V | Power out | HC-SR04 VCC, YF-S201 VCC |

> **Note:** GPIOs 34, 35 are input-only (no internal pull-up/pull-down).
> GPIOs 6–11 are connected to flash memory — do **not** use them.

---

## 3. Wiring Diagram

```
                    ┌──────────────────────────────────────────────┐
                    │              ESP32 DevKit                    │
                    │                                              │
 HC-SR04            │  GPIO 5 ──────────────────────── TRIG       │
 ┌──────────┐       │  GPIO18 ──┬── ECHO (via shifter if 5V)      │
 │VCC  ◄────┼──5V   │           │                                  │
 │GND  ◄────┼──GND  │  GND ─────┴── GND                           │
 │TRIG ◄────┼──────── GPIO5                                        │
 │ECHO ────►┼──────── GPIO18                                       │
 └──────────┘       │                                              │
                    │                                              │
 YF-S201 Flow       │  GPIO19 ─── Signal (yellow wire)            │
 ┌──────────┐       │  5V ──────── Red wire                        │
 │Red  ◄────┼──5V   │  GND ─────── Black wire                     │
 │Black ◄───┼──GND  │                                              │
 │Yellow───►┼──────── GPIO19 (INPUT_PULLUP)                        │
 └──────────┘       │                                              │
                    │                                              │
 Pressure Sensor    │  GPIO34 ─── Signal output (0.5–4.5V)        │
 ┌──────────┐       │  5V ──────── VCC (or 3.3V per datasheet)    │
 │VCC  ◄────┼──5V   │  GND ─────── GND                            │
 │GND  ◄────┼──GND  │                                              │
 │OUT  ────►┼──────── GPIO34 (ADC1_CH6)                           │
 └──────────┘       │                                              │
                    │                                              │
 ACS712-20A Module  │  GPIO35 ─── VIOUT (analog midpoint signal)  │
 ┌──────────┐       │  5V ──────── VCC                             │
 │VCC  ◄────┼──5V   │  GND ─────── GND                            │
 │GND  ◄────┼──GND  │                                              │
 │VIOUT────►┼──────── GPIO35 (ADC1_CH7)                           │
 │IP+  ◄────┼── Pump live wire IN                                  │
 │IP-  ────►┼── Pump live wire OUT                                 │
 └──────────┘       │                                              │
                    │                                              │
 Relay Module       │  GPIO2  ─── IN (control)                    │
 ┌──────────┐       │  5V ──────── VCC                             │
 │VCC  ◄────┼──5V   │  GND ─────── GND                            │
 │GND  ◄────┼──GND  │                                              │
 │IN   ◄────┼──────── GPIO2                                        │
 │COM  ◄────┼── Pump supply +                                      │
 │NO   ────►┼── Pump + terminal                                    │
 └──────────┘       │                                              │
                    └──────────────────────────────────────────────┘
```

> **Safety note:** Wire mains voltage (if pump runs on AC) only inside a proper
> enclosure with appropriate fusing.  The relay module separates the low-voltage
> ESP32 circuit from the high-voltage pump circuit.  The ACS712 provides galvanic
> isolation on the monitored wire.

---

## 4. Logic Level Considerations

The HC-SR04 ECHO pin outputs **5 V logic** on most modules.  The ESP32 GPIO input
is **not 5 V tolerant**.  Options:

| Option | Description |
|---|---|
| Resistor voltage divider | 1 kΩ + 2 kΩ from ECHO to GPIO → GND. Simple, passive. |
| Logic level shifter | Bidirectional 3.3/5 V shifter module. Cleaner; use if you have one. |
| 3.3 V HC-SR04 variant | Some modules (labelled "3.3 V" or "HC-SR04+") output 3.3 V logic directly. |

---

## 5. Required Libraries

Install via Arduino IDE Library Manager (Tools → Manage Libraries) or
`pio lib install` (PlatformIO):

| Library | Version | Install Name |
|---|---|---|
| ESP32 Arduino Core | ≥ 2.0.x | Board manager: `esp32 by Espressif Systems` |
| ArduinoJson | 6.x (NOT 7.x) | `ArduinoJson` |
| NewPing | Current | `NewPing` |
| WiFi | Built-in (ESP32 core) | — |
| HTTPClient | Built-in (ESP32 core) | — |

`platformio.ini` equivalent:
```ini
[env:esp32dev]
platform = espressif32
board = esp32dev
framework = arduino
lib_deps =
    bblanchon/ArduinoJson@^6.21.3
    teckel12/NewPing
```

---

## 6. Flashing Instructions

### Arduino IDE
1. Install ESP32 board package:
   - File → Preferences → Additional Board Manager URLs:  
     `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
   - Tools → Board Manager → search "esp32" → Install "esp32 by Espressif Systems"
2. Select board: Tools → Board → ESP32 Arduino → **ESP32 Dev Module**
3. Select port: Tools → Port → `COM_X` (Windows) or `/dev/ttyUSB0` (Linux/Mac)
4. Set Upload Speed: 921600
5. Set Flash Size: 4MB
6. Open `firmware/esp32_water_controller/esp32_water_controller.ino`
7. Edit WiFi credentials and `BACKEND_URL` constants at the top of the file
8. Click **Upload** (Ctrl+U)

### PlatformIO
```bash
cd firmware/esp32_water_controller
pio run --target upload
pio device monitor --baud 115200
```

### Troubleshooting Flash Issues
| Problem | Fix |
|---|---|
| "Failed to connect" | Hold BOOT button while clicking Upload, release after "Connecting…" |
| Port not detected | Install CP2102 or CH340 driver for your OS |
| Upload starts but fails mid-way | Lower Upload Speed to 460800 |

---

## 7. Testing Each Sensor Independently

### 7.1 Ultrasonic Sensor (HC-SR04)

Temporarily add to `loop()` (remove after testing):
```cpp
float d = readUltrasonicDistance();
Serial.printf("[TEST] Ultrasonic: %.1f cm\n", d);
delay(500);
```
Move your hand toward/away from the sensor and verify readings change proportionally.
Expected: stable readings within ±1 cm for a stationary target.

### 7.2 Flow Sensor (YF-S201)

Run water through the sensor; monitor `[SENSOR] Flow=` in serial output.
With no flow, value should be ≈0.00 LPM.
Increasing flow should show increasing LPM value.

### 7.3 Pressure Sensor

With no pipe pressure (sensor open to air), `pressure_bar` should be ≈0.00.
Squeeze the pressure port gently; value should increase.
Full-scale validation requires a reference pressure source.

### 7.4 Current Sensor (ACS712)

With no load through ACS712: `pump_current_a` ≈ 0.00 A.  
Slightly off-zero is normal (ADC noise); trim `CURRENT_MIDPOINT` if needed.
Power the pump while monitoring; value should match a clamp meter within ±10%.

### 7.5 Pump Relay

Temporarily add to `setup()`:
```cpp
// Test: toggle relay 3 times
for (int i = 0; i < 3; i++) {
    digitalWrite(PUMP_RELAY_PIN, HIGH);
    delay(500);
    digitalWrite(PUMP_RELAY_PIN, LOW);
    delay(500);
}
```
You should hear the relay click 3 times.  
**Never run the pump dry.** Ensure the pump has water before energising.

---

## 8. Connecting to the Backend

1. Ensure backend is running: `cd backend && python -m uvicorn main:app --host 0.0.0.0 --port 8000`
2. Find your server's LAN IP: `ip addr show` (Linux) or `ipconfig` (Windows)
3. Update `BACKEND_URL` in the firmware:
   ```cpp
   const char* BACKEND_URL = "http://192.168.1.100:8000";  // replace with your IP
   ```
4. Ensure `DEVICE_API_KEY` in firmware matches `DEVICE_API_KEY` in backend `.env`
5. Flash the updated firmware
6. Open serial monitor – you should see:
   ```
   [WIFI] Connected. IP: 192.168.1.105
   [TELEMETRY] Sent OK  HTTP=202
   [CMD] Fetch failed  HTTP=200  (empty array if no commands pending)
   ```
7. Open the frontend dashboard at `http://localhost:5173` and verify live data appears

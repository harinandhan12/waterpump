/**
 * @file    esp32_water_controller.ino
 * @brief   Smart Water Management & Pump Health Monitoring System – ESP32 Firmware
 * @version 1.0.0
 * @date    2024
 *
 * @hardware
 *   - ESP32 DevKit (38-pin or 30-pin variant)
 *   - HC-SR04 ultrasonic distance sensor (tank level)
 *   - YF-S201 Hall-effect flow sensor
 *   - 0–5 bar resistive/analog pressure sensor
 *   - ACS712-20A current sensor (5A / 20A module)
 *   - 5 V relay module (active HIGH) for pump control
 *
 * @dependencies
 *   - Arduino ESP32 core 2.x  (https://github.com/espressif/arduino-esp32)
 *   - ArduinoJson 6.x         (https://arduinojson.org/)
 *
 * @workflow
 *   loop() is fully non-blocking.  Timing is managed with millis() timestamps.
 *   A finite-state machine (FSM) drives pump control.
 *   Fault detection runs on-device; no connectivity is required for safety logic.
 *
 * @license MIT
 */

// ─────────────────────────────────────────────────────────────────────────────
//  Includes
// ─────────────────────────────────────────────────────────────────────────────
#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <NewPing.h>

// ─────────────────────────────────────────────────────────────────────────────
//  ▸ USER-CONFIGURABLE CONSTANTS
//    Edit these before flashing.  NEVER commit real credentials to source control.
// ─────────────────────────────────────────────────────────────────────────────

// ── WiFi / Network ──────────────────────────────────────────────────────────
const char* WIFI_SSID        = "YOUR_WIFI_SSID";
const char* WIFI_PASSWORD    = "YOUR_WIFI_PASSWORD";
const char* BACKEND_URL      = "http://192.168.1.100:8000";   // no trailing slash
const char* DEVICE_API_KEY   = "change-me-device-key-123";
const char* DEVICE_ID        = "esp32-water-controller-01";

// ── Pin Assignments ──────────────────────────────────────────────────────────
constexpr uint8_t TRIG_PIN          = 5;    // HC-SR04 trigger
constexpr uint8_t ECHO_PIN          = 18;   // HC-SR04 echo
constexpr uint8_t FLOW_SENSOR_PIN   = 19;   // YF-S201 pulse output (interrupt-capable)
constexpr uint8_t PRESSURE_PIN      = 34;   // ADC1 – analog pressure sensor
constexpr uint8_t CURRENT_PIN       = 35;   // ADC1 – ACS712 current sensor
constexpr uint8_t PUMP_RELAY_PIN    = 2;    // Relay IN (active HIGH)

// ── Tank Geometry ────────────────────────────────────────────────────────────
const float EMPTY_DIST_CM  = 15.0f;   // cm from sensor to water surface when tank is EMPTY
const float FULL_DIST_CM   =  7.0f;   // cm from sensor to water surface when tank is FULL
const float MIN_VALID_DIST =  2.0f;   // below this → sensor in water or blocked (invalid)
const float MAX_VALID_DIST = 400.0f;  // HC-SR04 practical max

// ── Control Thresholds ───────────────────────────────────────────────────────
const float LOW_LEVEL_THRESHOLD  = 20.0f;  // % – start pump when level drops below this
const float HIGH_LEVEL_THRESHOLD = 90.0f;  // % – stop pump when level reaches this
const float MAX_CURRENT_A        =  5.0f;  // A – electrical overload limit
const float MIN_FLOW_LPM         =  0.5f;  // LPM – below this after grace period → dry run

// ── Sensor Calibration ───────────────────────────────────────────────────────
// Pressure (0–5 bar, 12-bit ADC, 3.3 V reference):
//   Adjust zero_offset if your sensor reads non-zero at atmospheric pressure.
const float PRESSURE_ZERO_OFFSET = 0.0f;   // ADC counts at 0 bar; refine during calibration
const float PRESSURE_BAR_PER_LSB = (5.0f / 4095.0f); // full-scale span mapping

// ACS712-20A:
//   Mid-point ADC code (~2048 for 1.65 V mid-rail) and scale factor.
const int   CURRENT_MIDPOINT  = 2048;   // ADC reading at 0 A; trim with no-load measurement
const float CURRENT_SCALE     = 0.066f; // A per ADC LSB  (100 mV/A ÷ (3.3V/4095) = ~0.124 A/LSB)
                                        // ACS712-20A sensitivity = 100 mV/A → scale = 0.1/(3.3/4095)

// Flow sensor (YF-S201 default = 7.5 pulses/second per litre/minute):
//   calibration factor = pulses per litre (at our specific flow rate)
const float FLOW_PULSES_PER_LITRE = 450.0f;  // YF-S201 datasheet: ~450 pulses/L

// ── Timing ───────────────────────────────────────────────────────────────────
const unsigned long TELEMETRY_INTERVAL_MS   =  5000UL; // 5 s
const unsigned long COMMAND_POLL_INTERVAL_MS= 10000UL; // 10 s
const unsigned long GRACE_PERIOD_MS         = 10000UL; // allow pump to prime before fault checks
const unsigned long SENSOR_LOOP_INTERVAL_MS =   500UL; // 0.5 s sensor cadence
const unsigned long WIFI_CONNECT_TIMEOUT_MS =  15000UL;
constexpr unsigned long PULSIN_TIMEOUT_US   =  30000UL; // 30 ms ≈ 5 m range

// ── Ultrasonic Median Filter ──────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────────
//  State Machine Definition
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @enum ControllerState
 * @brief FSM states for pump control logic.
 *
 * IDLE    – Pump is OFF.  Monitor level; start when < LOW_LEVEL_THRESHOLD.
 * FILLING – Pump is ON.   Monitor sensors; detect overflow or fault conditions.
 * VERIFY  – Post-start check window (GRACE_PERIOD_MS).  Confirm flow/current/pressure.
 * FAULT   – Pump OFF.  A safety fault was detected.  Require manual reset via command.
 */
enum class ControllerState : uint8_t {
    IDLE    = 0,
    FILLING = 1,
    VERIFY  = 2,
    FAULT   = 3
};

/**
 * @enum OperatingMode
 * @brief Runtime mode toggled by backend command.
 *
 * AUTO   – FSM makes pump decisions automatically.
 * MANUAL – Pump state set exclusively via backend commands.
 */
enum class OperatingMode : uint8_t {
    AUTO   = 0,
    MANUAL = 1
};

NewPing sonar(TRIG_PIN, ECHO_PIN, static_cast<unsigned int>(MAX_VALID_DIST));

// ─────────────────────────────────────────────────────────────────────────────
//  Fault Code Definitions
// ─────────────────────────────────────────────────────────────────────────────
constexpr uint8_t FAULT_NONE           = 0x00;
constexpr uint8_t FAULT_DRY_RUN        = 0x01;
constexpr uint8_t FAULT_OVERLOAD       = 0x02;
constexpr uint8_t FAULT_SENSOR_FAIL    = 0x04;
constexpr uint8_t FAULT_COMM_TIMEOUT   = 0x08;  // informational; pump not stopped

// ─────────────────────────────────────────────────────────────────────────────
//  Global State
// ─────────────────────────────────────────────────────────────────────────────

// ── Sensor readings (updated every SENSOR_LOOP_INTERVAL_MS) ──────────────────
float g_tankDistanceCm   = -1.0f;  // HC-SR04 median distance
float g_tankLevelPct     = -1.0f;  // derived level %
float g_flowRateLpm      =  0.0f;  // YF-S201 LPM
float g_pressureBar      =  0.0f;  // pressure sensor bar
float g_pumpCurrentA     =  0.0f;  // ACS712 current
float g_lastValidDist    = -1.0f;  // previous valid ultrasonic reading

// ── Sensor health flags ───────────────────────────────────────────────────────
bool g_ultrasonicOk = true;
bool g_flowOk       = true;
bool g_pressureOk   = true;
bool g_currentOk    = true;

// ── FSM / control ────────────────────────────────────────────────────────────
ControllerState g_state       = ControllerState::IDLE;
OperatingMode   g_mode        = OperatingMode::AUTO;
bool            g_pumpOn      = false;
uint8_t         g_faultCode   = FAULT_NONE;
String          g_faultMsg    = "";
unsigned long   g_pumpStartMs = 0;  // millis() when pump last turned ON

// ── Flow pulse counter (ISR-updated) ─────────────────────────────────────────
volatile uint32_t g_flowPulseCount = 0;
uint32_t g_lastFlowPulses          = 0;
unsigned long g_lastFlowCalcMs     = 0;

// ── Timing cursors ───────────────────────────────────────────────────────────
unsigned long g_lastSensorReadMs  = 0;
unsigned long g_lastTelemetrySent = 0;
unsigned long g_lastCommandPollMs = 0;

// ── WiFi state ───────────────────────────────────────────────────────────────
bool          g_wifiConnected        = false;
unsigned long g_wifiLastAttemptMs    = 0;
unsigned long g_wifiRetryIntervalMs  = 5000UL;  // exponential backoff seed
constexpr unsigned long WIFI_RETRY_MAX_MS = 60000UL;

// ── Pending acknowledgement ───────────────────────────────────────────────────
String g_pendingAckCommandId = "";  // non-empty if we need to send an ACK

// ─────────────────────────────────────────────────────────────────────────────
//  Forward Declarations
// ─────────────────────────────────────────────────────────────────────────────
float       readUltrasonicDistance();
float       calculateTankLevel(float distance_cm);
void        IRAM_ATTR flowPulseISR();
float       calculateFlowRate();
float       readPressure();
float       readCurrent();
void        readAllSensors();
void        setPump(bool on, const char* reason = "");
void        runStateMachine();
void        enterFault(uint8_t code, const String& msg);
void        maintainWiFiConnection();
void        sendTelemetry();
void        fetchPendingCommands();
void        acknowledgeCommand(const String& commandId, bool success, const String& msg);
String      getTimestamp();
const char* stateToString(ControllerState s);
const char* modeToString(OperatingMode m);
void        sortFloatArray(float* arr, uint8_t len);

// ─────────────────────────────────────────────────────────────────────────────
//  ISR – Flow Sensor
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Interrupt service routine for YF-S201 flow sensor.
 *        Each rising edge = one Hall-effect pulse.
 *        IRAM_ATTR ensures ISR resides in IRAM for consistent latency.
 */
void IRAM_ATTR flowPulseISR() {
    g_flowPulseCount++;
}

// ─────────────────────────────────────────────────────────────────────────────
//  setup()
// ─────────────────────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    delay(500);  // allow serial to stabilise
    Serial.println(F("\n[BOOT] Smart Water Controller v1.0.0"));
    Serial.printf("[BOOT] Device ID: %s\n", DEVICE_ID);
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");

    // ── GPIO config ──────────────────────────────────────────────────────────
    pinMode(TRIG_PIN,        OUTPUT);
    pinMode(ECHO_PIN,        INPUT);
    pinMode(PUMP_RELAY_PIN,  OUTPUT);
    digitalWrite(TRIG_PIN,       LOW);
    digitalWrite(PUMP_RELAY_PIN, LOW);  // ensure pump is OFF at boot

    // ADC pins are configured implicitly by analogRead() but setting resolution
    // explicitly ensures 12-bit mode on both ADC1 channels.
    analogReadResolution(12);   // 0–4095
    analogSetAttenuation(ADC_11db);  // full 3.3 V range

    // ── Flow sensor ISR ───────────────────────────────────────────────────────
    pinMode(FLOW_SENSOR_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(FLOW_SENSOR_PIN),
                    flowPulseISR,
                    RISING);
    g_lastFlowCalcMs = millis();

    // ── Initial WiFi connect ──────────────────────────────────────────────────
    Serial.printf("[WIFI] Connecting to SSID: %s\n", WIFI_SSID);
    WiFi.setAutoReconnect(false);  // we manage reconnects manually
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

    unsigned long wifiStart = millis();
    while (WiFi.status() != WL_CONNECTED &&
           (millis() - wifiStart) < WIFI_CONNECT_TIMEOUT_MS) {
        delay(200);
        Serial.print('.');
    }

    if (WiFi.status() == WL_CONNECTED) {
        g_wifiConnected = true;
        g_wifiRetryIntervalMs = 5000UL;
        Serial.printf("\n[WIFI] Connected. IP: %s\n",
                      WiFi.localIP().toString().c_str());
    } else {
        Serial.println(F("\n[WIFI] Initial connect failed; will retry in loop."));
    }

    Serial.println(F("[BOOT] Initialisation complete. Entering main loop.\n"));
}

// ─────────────────────────────────────────────────────────────────────────────
//  loop()  – Non-Blocking Main Loop
// ─────────────────────────────────────────────────────────────────────────────

void loop() {
    unsigned long now = millis();

    // ── 1. Sensor reading & FSM (every 500 ms) ────────────────────────────────
    if (now - g_lastSensorReadMs >= SENSOR_LOOP_INTERVAL_MS) {
        readAllSensors();
        runStateMachine();
        g_lastSensorReadMs = now;
    }

    // ── 2. Telemetry upload (every 5 s, WiFi required) ───────────────────────
    if (g_wifiConnected &&
        (now - g_lastTelemetrySent >= TELEMETRY_INTERVAL_MS)) {
        sendTelemetry();
        g_lastTelemetrySent = now;
    }

    // ── 3. Command polling (every 10 s, WiFi required) ───────────────────────
    if (g_wifiConnected &&
        (now - g_lastCommandPollMs >= COMMAND_POLL_INTERVAL_MS)) {
        fetchPendingCommands();
        g_lastCommandPollMs = now;
    }

    // ── 4. WiFi reconnect (non-blocking check) ────────────────────────────────
    maintainWiFiConnection();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Sensor Reading Functions
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Read the HC-SR04 distance using NewPing.
 *
 * @return Median distance in centimetres, or -1 on failure.
 */
float readUltrasonicDistance() {
    const unsigned int distance = sonar.ping_cm();
    return distance == 0 ? -1.0f : static_cast<float>(distance);
}

/**
 * @brief Convert ultrasonic distance to tank fill percentage.
 *
 * @details
 *   Linear interpolation between EMPTY_DIST_CM (0%) and FULL_DIST_CM (100%).
 *   Clamps result to [0, 100].  Returns -1 for invalid input.
 *
 * @param distance_cm  Raw distance from readUltrasonicDistance().
 * @return Level in percent [0–100], or -1 if input is invalid.
 */
float calculateTankLevel(float distance_cm) {
    // Reject invalid / out-of-range readings
    if (distance_cm <= 0.0f ||
        distance_cm < MIN_VALID_DIST ||
        distance_cm > MAX_VALID_DIST) {
        return -1.0f;
    }

    // Interpolate: closer to sensor → higher level (distance decreases as tank fills)
    float level = (EMPTY_DIST_CM - distance_cm) /
                  (EMPTY_DIST_CM - FULL_DIST_CM) * 100.0f;

    // Clamp to valid range
    if (level < 0.0f)   level = 0.0f;
    if (level > 100.0f) level = 100.0f;

    return level;
}

/**
 * @brief Calculate flow rate in litres per minute from pulse count delta.
 *
 * @details
 *   Reads the volatile pulse counter atomically, computes pulses since last
 *   call, and converts to LPM using FLOW_PULSES_PER_LITRE and elapsed time.
 *
 * @return Flow rate in LPM.
 */
float calculateFlowRate() {
    unsigned long now = millis();

    // Atomically read and reset counter
    noInterrupts();
    uint32_t currentPulses = g_flowPulseCount;
    g_flowPulseCount = 0;
    interrupts();

    unsigned long elapsed = now - g_lastFlowCalcMs;
    g_lastFlowCalcMs = now;

    if (elapsed == 0) return g_flowRateLpm;  // avoid divide-by-zero

    // Pulses in this window → litres → scaled to per-minute
    float litres  = (float)currentPulses / FLOW_PULSES_PER_LITRE;
    float minutes = (float)elapsed / 60000.0f;
    float lpm     = (minutes > 0.0f) ? (litres / minutes) : 0.0f;

    return lpm;
}

/**
 * @brief Read analog pressure sensor and convert to bar.
 *
 * @details
 *   Takes 8 averaged ADC readings to reduce noise.
 *   Applies zero-offset correction, then linear scaling.
 *   Clamps to [0, 5] bar physical range.
 *
 * @return Pressure in bar.
 */
float readPressure() {
    uint32_t sum = 0;
    const uint8_t SAMPLES = 8;
    for (uint8_t i = 0; i < SAMPLES; i++) {
        sum += analogRead(PRESSURE_PIN);
    }
    float raw = (float)(sum / SAMPLES);

    // Subtract zero offset, then scale to bar
    float bar = (raw - PRESSURE_ZERO_OFFSET) * PRESSURE_BAR_PER_LSB;

    // Clamp to physical range
    if (bar < 0.0f) bar = 0.0f;
    if (bar > 5.0f) bar = 5.0f;

    return bar;
}

/**
 * @brief Read ACS712 current sensor and convert to amperes.
 *
 * @details
 *   ACS712-20A: sensitivity = 100 mV/A, midpoint = Vcc/2 (≈1.65 V → ADC ≈ 2048).
 *   Takes 16 averaged ADC readings.
 *   Returns absolute value (direction unimportant for pump monitoring).
 *
 * @return RMS-approximated current magnitude in amperes.
 */
float readCurrent() {
    uint32_t sum = 0;
    const uint8_t SAMPLES = 16;
    for (uint8_t i = 0; i < SAMPLES; i++) {
        sum += analogRead(CURRENT_PIN);
    }
    float raw = (float)(sum / SAMPLES);

    // Deviation from mid-point × scale factor
    float current = fabsf((raw - (float)CURRENT_MIDPOINT) * CURRENT_SCALE);
    return current;
}

/**
 * @brief Orchestrate all sensor reads and update global state variables.
 */
void readAllSensors() {
    // ── Ultrasonic / Tank Level ───────────────────────────────────────────────
    float dist = readUltrasonicDistance();
    if (dist >= MIN_VALID_DIST && dist <= MAX_VALID_DIST) {
        g_tankDistanceCm = dist;
        g_lastValidDist  = dist;
        g_ultrasonicOk   = true;
    } else {
        // Invalid reading – retain last known good value
        g_ultrasonicOk   = false;
        g_tankDistanceCm = g_lastValidDist;  // -1 if never had a valid reading
        Serial.println(F("[SENSOR] Ultrasonic: invalid reading, using last valid."));
    }

    g_tankLevelPct = calculateTankLevel(g_tankDistanceCm);

    // ── Flow Rate ─────────────────────────────────────────────────────────────
    g_flowRateLpm = calculateFlowRate();
    g_flowOk      = true;  // YF-S201 doesn't signal errors; assume OK

    // ── Pressure ──────────────────────────────────────────────────────────────
    g_pressureBar = readPressure();
    g_pressureOk  = (g_pressureBar >= 0.0f && g_pressureBar <= 5.0f);

    // ── Current ───────────────────────────────────────────────────────────────
    g_pumpCurrentA = readCurrent();
    g_currentOk    = true;  // reading itself is always valid; fault logic elsewhere

    // ── Serial Debug ──────────────────────────────────────────────────────────
    Serial.printf("[SENSOR] Dist=%.1f cm  Level=%.1f%%  Flow=%.2f LPM  "
                  "Pressure=%.2f bar  Current=%.2f A  Pump=%s\n",
                  g_tankDistanceCm, g_tankLevelPct,
                  g_flowRateLpm,    g_pressureBar,
                  g_pumpCurrentA,   g_pumpOn ? "ON" : "OFF");
}

// ─────────────────────────────────────────────────────────────────────────────
//  Pump Control
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Turn the pump relay ON or OFF and record state.
 *
 * @param on     true = energise relay (pump ON); false = de-energise (pump OFF).
 * @param reason Human-readable reason (for serial log and telemetry).
 */
void setPump(bool on, const char* reason) {
    if (on == g_pumpOn) return;  // no change needed

    g_pumpOn = on;
    digitalWrite(PUMP_RELAY_PIN, on ? HIGH : LOW);

    if (on) {
        g_pumpStartMs = millis();
    }

    Serial.printf("[PUMP] %s – reason: %s\n", on ? "ON" : "OFF", reason);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Finite State Machine
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Transition to FAULT state: pump OFF, record code and message.
 */
void enterFault(uint8_t code, const String& msg) {
    setPump(false, "FAULT");
    g_state     = ControllerState::FAULT;
    g_faultCode = code;
    g_faultMsg  = msg;
    Serial.printf("[FSM] FAULT: code=0x%02X  msg=%s\n", code, msg.c_str());
}

/**
 * @brief Run one iteration of the pump control FSM.
 *
 * @details
 *   Called every SENSOR_LOOP_INTERVAL_MS from loop().
 *   State transitions:
 *
 *     IDLE ──(level < LOW)──► FILLING/VERIFY
 *     FILLING ──(level >= HIGH)──► IDLE
 *     FILLING/VERIFY ──(fault)──► FAULT
 *     VERIFY ──(grace elapsed, flow OK)──► FILLING
 *     FAULT ──(manual reset command)──► IDLE  [handled in fetchPendingCommands]
 */
void runStateMachine() {
    // Electrical overload check applies in ALL states when pump is running
    if (g_pumpOn && g_pumpCurrentA > MAX_CURRENT_A) {
        enterFault(FAULT_OVERLOAD,
                   "Current " + String(g_pumpCurrentA, 2) + " A exceeds limit " +
                   String(MAX_CURRENT_A, 2) + " A");
        return;
    }

    switch (g_state) {

        // ── IDLE ─────────────────────────────────────────────────────────────
        case ControllerState::IDLE:
            // Pump must be OFF
            setPump(false, "IDLE state");

            if (g_mode == OperatingMode::AUTO &&
                g_tankLevelPct >= 0.0f &&               // valid level reading
                g_tankLevelPct < LOW_LEVEL_THRESHOLD) {

                Serial.printf("[FSM] IDLE → VERIFY  (level=%.1f%% < threshold=%.1f%%)\n",
                              g_tankLevelPct, LOW_LEVEL_THRESHOLD);
                setPump(true, "Level below LOW threshold");
                g_state = ControllerState::VERIFY;
            }
            break;

        // ── VERIFY ───────────────────────────────────────────────────────────
        // Pump just started; wait GRACE_PERIOD_MS before evaluating flow/pressure.
        case ControllerState::VERIFY: {
            unsigned long elapsed = millis() - g_pumpStartMs;

            // Overflow protection even during grace period
            if (g_tankLevelPct >= HIGH_LEVEL_THRESHOLD) {
                Serial.println(F("[FSM] VERIFY → IDLE  (overflow protection)"));
                setPump(false, "Tank full during VERIFY");
                g_state = ControllerState::IDLE;
                break;
            }

            if (elapsed >= GRACE_PERIOD_MS) {
                // Grace period elapsed – evaluate sensor data
                if (g_flowRateLpm < MIN_FLOW_LPM) {
                    enterFault(FAULT_DRY_RUN,
                               "Flow " + String(g_flowRateLpm, 2) +
                               " LPM < min " + String(MIN_FLOW_LPM, 2) + " LPM – possible dry run");
                } else {
                    Serial.printf("[FSM] VERIFY → FILLING  (flow=%.2f LPM OK)\n", g_flowRateLpm);
                    g_state = ControllerState::FILLING;
                }
            }
            break;
        }

        // ── FILLING ──────────────────────────────────────────────────────────
        case ControllerState::FILLING:
            // Pump ON – monitor for completion and faults

            // Tank full?
            if (g_tankLevelPct >= HIGH_LEVEL_THRESHOLD) {
                Serial.printf("[FSM] FILLING → IDLE  (level=%.1f%% >= %.1f%%)\n",
                              g_tankLevelPct, HIGH_LEVEL_THRESHOLD);
                setPump(false, "Tank reached HIGH threshold");
                g_state = ControllerState::IDLE;
                break;
            }

            // Dry-run detection (flow dropped while pump running)
            if (g_flowRateLpm < MIN_FLOW_LPM) {
                enterFault(FAULT_DRY_RUN,
                           "Flow dropped to " + String(g_flowRateLpm, 2) +
                           " LPM during FILLING – possible dry run");
                break;
            }

            // Ultrasonic failure (tank level unknown)
            if (!g_ultrasonicOk && g_lastValidDist < 0.0f) {
                // No valid level reading at all – halt safely
                enterFault(FAULT_SENSOR_FAIL,
                           "Ultrasonic sensor failure – cannot monitor tank level");
                break;
            }

            // Still filling normally
            Serial.printf("[FSM] FILLING  level=%.1f%%  flow=%.2f LPM\n",
                          g_tankLevelPct, g_flowRateLpm);
            break;

        // ── FAULT ────────────────────────────────────────────────────────────
        case ControllerState::FAULT:
            // Pump must remain OFF; wait for manual reset command from backend
            setPump(false, "FAULT – awaiting manual reset");
            // (Reset handled in fetchPendingCommands → "reset_fault" command)
            break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  WiFi Management
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Non-blocking WiFi reconnect with exponential backoff.
 *
 * @details
 *   Checks WiFi status.  If disconnected and backoff interval has elapsed,
 *   initiates a new WiFi.begin().  Doubles retry interval up to WIFI_RETRY_MAX_MS.
 *   Does NOT block the sensor loop.
 */
void maintainWiFiConnection() {
    bool connected = (WiFi.status() == WL_CONNECTED);

    if (connected && !g_wifiConnected) {
        g_wifiConnected       = true;
        g_wifiRetryIntervalMs = 5000UL;  // reset backoff on successful connect
        Serial.printf("[WIFI] Reconnected. IP: %s\n",
                      WiFi.localIP().toString().c_str());
    } else if (!connected) {
        g_wifiConnected = false;
        unsigned long now = millis();

        if (now - g_wifiLastAttemptMs >= g_wifiRetryIntervalMs) {
            Serial.printf("[WIFI] Attempting reconnect (interval=%lu ms)…\n",
                          g_wifiRetryIntervalMs);
            WiFi.disconnect(true);
            WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
            g_wifiLastAttemptMs = now;

            // Exponential backoff: double interval, cap at maximum
            g_wifiRetryIntervalMs = min(g_wifiRetryIntervalMs * 2, WIFI_RETRY_MAX_MS);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  HTTP / Backend Communication
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Build and POST telemetry JSON to the backend.
 *
 * @details
 *   Constructs a StaticJsonDocument, serialises to string, sends via HTTP POST.
 *   Non-blocking at the WiFi layer – if HTTPClient fails quickly the sensor
 *   loop is not affected.  Telemetry continues even in FAULT state so the
 *   operator sees real-time sensor data.
 */
void sendTelemetry() {
    // ── JSON document ─────────────────────────────────────────────────────────
    StaticJsonDocument<512> doc;
    doc["device_id"]        = DEVICE_ID;
    doc["timestamp"]        = getTimestamp();
    doc["tank_distance_cm"] = g_tankDistanceCm;
    if (g_tankLevelPct >= 0.0f) {
        doc["tank_level_percent"] = g_tankLevelPct;
    }
    doc["flow_rate_lpm"]    = g_flowRateLpm;
    doc["pressure_bar"]     = g_pressureBar;
    doc["pump_current_a"]   = g_pumpCurrentA;
    doc["pump_on"]          = g_pumpOn;
    doc["mode"]             = modeToString(g_mode);
    doc["controller_state"] = stateToString(g_state);

    if (g_faultCode != FAULT_NONE) {
        doc["fault_code"] = g_faultCode;
        doc["fault_msg"]  = g_faultMsg;
    }

    JsonObject sensorStatus = doc.createNestedObject("sensor_status");
    sensorStatus["ultrasonic"] = g_ultrasonicOk ? "OK" : "FAIL";
    sensorStatus["flow"]       = g_flowOk       ? "OK" : "FAIL";
    sensorStatus["pressure"]   = g_pressureOk   ? "OK" : "FAIL";
    sensorStatus["current"]    = g_currentOk    ? "OK" : "FAIL";

    String payload;
    payload.reserve(384);
    serializeJson(doc, payload);

    // ── HTTP POST ─────────────────────────────────────────────────────────────
    String url = String(BACKEND_URL) + "/api/v1/telemetry";
    HTTPClient http;
    http.begin(url);
    http.addHeader("Content-Type",  "application/json");
    http.addHeader("X-API-Key",     DEVICE_API_KEY);
    http.addHeader("X-Device-ID",   DEVICE_ID);
    http.setTimeout(4000);  // 4 s timeout – keeps loop responsive

    int code = http.POST(payload);

    if (code == HTTP_CODE_OK || code == HTTP_CODE_CREATED || code == 202) {
        Serial.printf("[TELEMETRY] Sent OK  HTTP=%d\n", code);
    } else {
        Serial.printf("[TELEMETRY] Failed  HTTP=%d  err=%s\n",
                      code, http.errorToString(code).c_str());
    }

    http.end();
}

/**
 * @brief Poll backend for pending commands and execute them.
 *
 * @details
 *   GET /api/v1/devices/<DEVICE_ID>/commands/pending
 *   Expected response: JSON array of command objects.
 *   Supported commands:
 *     "pump_on"      – MANUAL mode: turn pump ON
 *     "pump_off"     – MANUAL mode: turn pump OFF
 *     "set_mode_auto"   – Switch to AUTO mode
 *     "set_mode_manual" – Switch to MANUAL mode
 *     "reset_fault"     – Clear FAULT state, return to IDLE
 *     "reboot"          – Software reset via ESP.restart()
 */
void fetchPendingCommands() {
    String url = String(BACKEND_URL) +
                 "/api/v1/devices/" + String(DEVICE_ID) + "/commands/pending";

    HTTPClient http;
    http.begin(url);
    http.addHeader("X-API-Key",   DEVICE_API_KEY);
    http.addHeader("X-Device-ID", DEVICE_ID);
    http.setTimeout(5000);

    int code = http.GET();

    if (code != HTTP_CODE_OK) {
        Serial.printf("[CMD] Fetch failed  HTTP=%d\n", code);
        http.end();
        return;
    }

    String body = http.getString();
    http.end();

    // ── Parse JSON array ──────────────────────────────────────────────────────
    DynamicJsonDocument cmdDoc(1024);
    DeserializationError err = deserializeJson(cmdDoc, body);
    if (err) {
        Serial.printf("[CMD] JSON parse error: %s\n", err.c_str());
        return;
    }

    JsonArray commands = cmdDoc.as<JsonArray>();
    for (JsonObject cmd : commands) {
        const char* action    = cmd["command_type"] | "";
        const char* commandId = cmd["command_id"] | "";
        bool success = true;
        String ackMsg = "OK";

        Serial.printf("[CMD] Executing: %s  id=%s\n", action, commandId);

        if (strcmp(action, "PUMP_ON") == 0) {
            if (g_mode == OperatingMode::MANUAL) {
                setPump(true, "Backend command: pump_on");
            } else {
                success = false;
                ackMsg  = "Cannot execute pump_on in AUTO mode";
            }

        } else if (strcmp(action, "PUMP_OFF") == 0) {
            if (g_mode == OperatingMode::MANUAL) {
                setPump(false, "Backend command: pump_off");
            } else {
                success = false;
                ackMsg  = "Cannot execute pump_off in AUTO mode";
            }

        } else if (strcmp(action, "SET_MODE_AUTO") == 0) {
            g_mode = OperatingMode::AUTO;
            Serial.println(F("[CMD] Switched to AUTO mode"));

        } else if (strcmp(action, "SET_MODE_MANUAL") == 0) {
            g_mode = OperatingMode::MANUAL;
            setPump(false, "Mode switched to MANUAL – pump defaulted OFF");
            Serial.println(F("[CMD] Switched to MANUAL mode"));

        } else if (strcmp(action, "reset_fault") == 0) {
            if (g_state == ControllerState::FAULT) {
                g_faultCode = FAULT_NONE;
                g_faultMsg  = "";
                g_state     = ControllerState::IDLE;
                Serial.println(F("[CMD] Fault cleared – returning to IDLE"));
            } else {
                ackMsg = "No active fault to reset";
            }

        } else if (strcmp(action, "reboot") == 0) {
            Serial.println(F("[CMD] Rebooting in 1 second…"));
            acknowledgeCommand(commandId, true, "Rebooting");
            delay(1000);
            ESP.restart();

        } else {
            success = false;
            ackMsg  = String("Unknown command: ") + action;
            Serial.printf("[CMD] Unknown command: %s\n", action);
        }

        // Acknowledge the command (best-effort)
        if (strlen(commandId) > 0) {
            acknowledgeCommand(commandId, success, ackMsg);
        }
    }
}

/**
 * @brief Send a command acknowledgement back to the backend.
 *
 * @param commandId  Unique ID of the command being acknowledged.
 * @param success    Whether the command executed successfully.
 * @param msg        Human-readable status message.
 */
void acknowledgeCommand(const String& commandId,
                        bool          success,
                        const String& msg) {
    StaticJsonDocument<256> ackDoc;
    ackDoc["device_reported_state"] = success ? msg : String("ERROR: ") + msg;

    String payload;
    serializeJson(ackDoc, payload);

    String url = String(BACKEND_URL) + "/api/v1/devices/" + String(DEVICE_ID) +
                 "/commands/" + commandId + "/ack";
    HTTPClient http;
    http.begin(url);
    http.addHeader("Content-Type",  "application/json");
    http.addHeader("X-API-Key",     DEVICE_API_KEY);
    http.addHeader("X-Device-ID",   DEVICE_ID);
    http.setTimeout(4000);

    int code = http.POST(payload);
    Serial.printf("[ACK] command_id=%s  success=%s  HTTP=%d\n",
                  commandId.c_str(), success ? "true" : "false", code);
    http.end();
}

String getTimestamp() {
    time_t now = time(nullptr);
    if (now < 1600000000) {
        return "2024-01-01T00:00:00Z";
    }

    struct tm utc;
    gmtime_r(&now, &utc);
    char timestamp[25];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return String(timestamp);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Utility Functions
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief In-place insertion sort for a small float array.
 *
 * @param arr  Pointer to first element.
 * @param len  Number of elements.
 */
void sortFloatArray(float* arr, uint8_t len) {
    for (uint8_t i = 1; i < len; i++) {
        float key = arr[i];
        int8_t j  = (int8_t)(i - 1);
        while (j >= 0 && arr[j] > key) {
            arr[j + 1] = arr[j];
            j--;
        }
        arr[j + 1] = key;
    }
}

/**
 * @brief Return a string representation of a ControllerState value.
 */
const char* stateToString(ControllerState s) {
    switch (s) {
        case ControllerState::IDLE:    return "IDLE";
        case ControllerState::FILLING: return "FILLING";
        case ControllerState::VERIFY:  return "VERIFY";
        case ControllerState::FAULT:   return "FAULT";
        default:                        return "UNKNOWN";
    }
}

/**
 * @brief Return a string representation of an OperatingMode value.
 */
const char* modeToString(OperatingMode m) {
    switch (m) {
        case OperatingMode::AUTO:   return "AUTO";
        case OperatingMode::MANUAL: return "MANUAL";
        default:                     return "UNKNOWN";
    }
}

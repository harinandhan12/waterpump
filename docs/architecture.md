# System Architecture

## Smart Water Management & Pump Health Monitoring System

---

## 1. High-Level Overview

```
┌─────────────────────────────────────────────────────────────────────────┐
│                         FIELD HARDWARE                                  │
│                                                                         │
│  ┌──────────────┐   pulse ISR  ┌─────────────────────────────────────┐ │
│  │  YF-S201     │─────────────►│                                     │ │
│  │  Flow Sensor │              │         ESP32 DevKit                │ │
│  └──────────────┘              │                                     │ │
│                                │  ┌───────────┐  ┌───────────────┐  │ │
│  ┌──────────────┐   pulseIn()  │  │  Median   │  │  FSM Engine   │  │ │
│  │  HC-SR04     │─────────────►│  │  Filter   │  │  IDLE/FILL/   │  │ │
│  │  Ultrasonic  │              │  │  (5 smpl) │  │  VERIFY/FAULT │  │ │
│  └──────────────┘              │  └───────────┘  └───────────────┘  │ │
│                                │                                     │ │
│  ┌──────────────┐   analogRead │  ┌────────────────────────────┐    │ │
│  │  Pressure    │─────────────►│  │     WiFi (non-blocking)    │    │ │
│  │  Sensor      │              │  │  reconnect + backoff       │    │ │
│  └──────────────┘              │  └──────────────┬─────────────┘    │ │
│                                │                 │ HTTP/REST         │ │
│  ┌──────────────┐   analogRead │  ┌──────────────┴─────────────┐    │ │
│  │  ACS712      │─────────────►│  │  sendTelemetry()  (5 s)    │    │ │
│  │  Current     │              │  │  fetchCommands()  (10 s)   │    │ │
│  └──────────────┘              │  │  acknowledgeCmd()           │    │ │
│                                │  └────────────────────────────┘    │ │
│  ┌──────────────┐   GPIO OUT   │                                     │ │
│  │  Pump Relay  │◄────────────-│  PUMP_RELAY_PIN (active HIGH)      │ │
│  └──────────────┘              └─────────────────────────────────────┘ │
└─────────────────────────────────────────────────────────────────────────┘
                          │  WiFi / LAN
                          ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                         BACKEND SERVER                                  │
│                                                                         │
│  ┌────────────────────────────────────────────────────────────────────┐ │
│  │  FastAPI (Python 3.11)                                             │ │
│  │                                                                    │ │
│  │  POST /api/v1/telemetry         (from ESP32 every 5 s)            │ │
│  │  GET  /api/v1/devices/{id}/commands/pending (ESP32 every 10 s)   │ │
│  │  POST /api/v1/devices/{id}/commands/{cmd}/ack                    │ │
│  │  GET  /api/v1/status            (frontend dashboard)              │ │
│  │  POST /api/v1/commands          (frontend issues commands)        │ │
│  │  WS   /ws/dashboard             (real-time push to browser)       │ │
│  └───────────────────────┬────────────────────────────────────────────┘ │
│                          │                                              │
│  ┌───────────────────────┴──────────────────────────────────────────┐  │
│  │  SQLite / PostgreSQL  (telemetry time-series, commands, audit)   │  │
│  └──────────────────────────────────────────────────────────────────┘  │
└──────────────────────────────────┬──────────────────────────────────────┘
                                   │  WebSocket (ws://)
                                   ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                         FRONTEND (React + Vite)                         │
│                                                                         │
│   Dashboard  │  Tank Level Gauge  │  Pump Control  │  Fault Alerts      │
│   Real-time charts via WebSocket push                                   │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## 2. Component Descriptions

### 2.1 ESP32 Firmware (`firmware/`)
The microcontroller is the **safety-critical layer**.  All pump protection logic
(dry-run, overload, overflow) executes locally in the FSM — network connectivity
is optional for safety.

| Sub-component | Responsibility |
|---|---|
| Sensor loop (500 ms) | Reads all sensors, applies median filter, calculates derived values |
| FSM engine | Drives pump relay through `IDLE → VERIFY → FILLING → FAULT` lifecycle |
| WiFi manager | Non-blocking connect/reconnect with exponential backoff |
| Telemetry sender (5 s) | Serialises sensor + state snapshot as JSON, POSTs to backend |
| Command poller (10 s) | GETs pending commands, executes, acknowledges |

### 2.2 Backend (`backend/`)
A stateless REST + WebSocket server built with **FastAPI**.

| Component | Detail |
|---|---|
| REST API | Receives telemetry, stores to DB, serves status, accepts commands |
| Command queue | Stores operator commands for ESP32 to fetch (pull-based delivery) |
| WebSocket broadcaster | Pushes latest telemetry to all connected browser sessions |
| Auth middleware | Validates `X-API-Key` header for device; `Authorization: Bearer` for frontend |
| Simulation mode | Generates synthetic sensor data when no real ESP32 is connected |

### 2.3 Frontend (`frontend/`)
A **React + Vite** single-page application served as static files.

| Feature | Implementation |
|---|---|
| Real-time dashboard | WebSocket connection to `/ws/dashboard` |
| Tank gauge | SVG-animated fill level indicator |
| Pump control | Buttons call `POST /api/v1/commands` |
| Alert panel | Displays fault code + message; shows reset button |
| History charts | Line charts from `GET /api/v1/telemetry/history` |

### 2.4 Database
- **SQLite** (default, zero-config, stored in `data/water_management.db`)
- **PostgreSQL** (production; change `DATABASE_URL` in `.env`)

Tables:
- `telemetry_readings` – time-series sensor snapshots
- `commands` – command queue with status (`PENDING`, `DELIVERED`, `ACKNOWLEDGED`)
- `fault_log` – historical fault events with timestamps

---

## 3. Data Flow

```
[ESP32] ──POST /api/v1/telemetry──► [Backend] ──INSERT──► [DB]
                                        │
                                        └──broadcast──► [WebSocket clients]
                                                             │
                                                      [Browser dashboard]

[Browser] ──POST /api/v1/commands──► [Backend] ──INSERT (PENDING)──► [DB]
                                         │
[ESP32] ──GET /api/v1/devices/{id}/commands/pending──► [Backend] ──SELECT──► ESP32 executes
                                         │
[ESP32] ──POST /api/v1/devices/{id}/commands/{cmd}/ack──► [Backend] ──UPDATE status──► [DB]
```

### Telemetry Cadence
| Layer | Interval |
|---|---|
| Sensor reading | 500 ms |
| Telemetry POST (ESP32 → backend) | 5 s |
| Command poll (ESP32 → backend) | 10 s |
| WebSocket push (backend → browser) | On each telemetry receipt |

---

## 4. Security Model

| Concern | Mechanism |
|---|---|
| Device authentication | `X-API-Key: <DEVICE_API_KEY>` header on all device requests |
| Frontend authentication | `Authorization: Bearer <FRONTEND_API_TOKEN>` on all frontend API requests |
| Transport encryption | **HTTP in LAN deployment** (acceptable for home/industrial LAN); enable TLS (HTTPS/WSS) via a reverse proxy (nginx + Let's Encrypt) for internet-facing deployments |
| Secret storage (firmware) | Constants in `.ino` file — **never commit to public repos**; use Arduino build secrets or NVS for production |
| Secret storage (backend) | `.env` file excluded from version control via `.gitignore` |
| Rate limiting | Apply at nginx/API gateway layer; backend trusts upstream rate limiter |
| Input validation | All telemetry fields validated server-side before DB write |

> **Warning:** The default API keys in `.env.example` and `esp32_water_controller.ino`
> are placeholders. **Change them before deployment.**

---

## 5. Offline Resilience

The system is designed so that **loss of network connectivity does not compromise water system safety**.

| Scenario | Behaviour |
|---|---|
| WiFi drops | ESP32 continues reading sensors and running FSM; WiFi reconnects with backoff |
| Backend unreachable | Telemetry POST silently fails; pump decisions are unaffected |
| Backend restarts | ESP32 reconnects automatically on next poll interval |
| Power loss (ESP32) | Pump relay defaults to OFF (active HIGH relay de-energises) |
| Power loss (backend) | DB persists to disk; dashboard reconnects when backend resumes |
| Ultrasonic sensor failure | Last valid reading used; FAULT triggered if no valid reading ever received |

# Smart Water Management & Pump Health Monitoring System

A full-stack IoT platform for real-time water tank monitoring and intelligent pump control.  
The system combines an ESP32 firmware controller, a FastAPI backend, and a React dashboard
to provide automated water level management, pump health monitoring, and fault alerting.

---

## Features

- 🌊 **Real-time tank level monitoring** via HC-SR04 ultrasonic sensor with 5-sample median filtering
- 💧 **Flow rate measurement** with YF-S201 Hall-effect sensor (interrupt-driven)
- ⚡ **Electrical overload protection** via ACS712 current sensor
- 🔧 **Dry-run & overflow detection** with configurable thresholds
- 🤖 **AUTO / MANUAL operating modes** switchable from the web dashboard
- 📡 **WebSocket real-time dashboard** with live charts and fault alerts
- 🔁 **Non-blocking WiFi with exponential backoff** — safety logic works offline
- 💻 **Simulation mode** — full-stack development without physical hardware

---

## Project Structure

```
smart-water-management/
├── firmware/
│   └── esp32_water_controller/
│       ├── esp32_water_controller.ino   # Main ESP32 Arduino sketch
│       └── calibration_procedure.md     # Firmware-level calibration guide
├── backend/
│   ├── main.py                          # FastAPI application entry point
│   ├── models.py                        # SQLAlchemy ORM models
│   ├── database.py                      # DB connection and session management
│   ├── routers/
│   │   ├── telemetry.py                 # POST /api/v1/telemetry
│   │   ├── commands.py                  # Command queue endpoints
│   │   └── status.py                    # GET /api/v1/status, history, faults
│   ├── simulator.py                     # Synthetic sensor data generator
│   └── requirements.txt
├── frontend/
│   ├── src/
│   │   ├── App.jsx
│   │   ├── components/
│   │   │   ├── TankGauge.jsx            # SVG animated tank level
│   │   │   ├── SensorCards.jsx          # Flow, pressure, current cards
│   │   │   ├── PumpControl.jsx          # ON/OFF/mode buttons
│   │   │   └── FaultAlert.jsx           # Fault display + reset button
│   │   └── hooks/
│   │       └── useWebSocket.js          # WS connection management
│   ├── package.json
│   └── vite.config.js
├── docs/
│   ├── architecture.md                  # System design & data flow
│   ├── api.md                           # Complete REST + WebSocket reference
│   ├── hardware-integration.md          # BOM, wiring, flashing, testing
│   └── calibration.md                   # Detailed sensor calibration guide
├── data/                                # SQLite database (auto-created)
├── .env.example                         # Environment variable template
├── .gitignore
└── README.md                            # This file
```

---

## Quick Start

### Prerequisites

| Tool | Minimum Version | Install |
|---|---|---|
| Python | 3.11+ | [python.org](https://python.org) |
| Node.js | 18+ | [nodejs.org](https://nodejs.org) |
| Git | 2.x | [git-scm.com](https://git-scm.com) |
| Arduino IDE | 2.x (hardware only) | [arduino.cc](https://www.arduino.cc/en/software) |

---

### Installation (Linux / macOS)

```bash
# 1. Clone the repository
git clone https://github.com/your-org/smart-water-management.git
cd smart-water-management

# 2. Copy environment template
cp .env.example .env
# Edit .env with your preferred text editor — change API keys at minimum

# 3. Backend setup
cd backend
python3 -m venv venv
source venv/bin/activate
pip install -r requirements.txt
cd ..

# 4. Frontend setup
cd frontend
npm install
cd ..
```

---

### Installation (Windows)

```powershell
# 1. Clone the repository
git clone https://github.com/your-org/smart-water-management.git
cd smart-water-management

# 2. Copy environment template
copy .env.example .env
# Edit .env with Notepad or VS Code

# 3. Backend setup
cd backend
python -m venv venv
venv\Scripts\activate
pip install -r requirements.txt
cd ..

# 4. Frontend setup
cd frontend
npm install
cd ..
```

---

### Running in Simulation Mode (no hardware required)

Simulation mode generates realistic synthetic sensor data so you can develop and
test the full stack without any physical ESP32 or sensors.

**Terminal 1 – Backend:**
```bash
cd backend
source venv/bin/activate      # Linux/Mac
# venv\Scripts\activate       # Windows
uvicorn main:app --host 0.0.0.0 --port 8000 --reload
```

The backend reads `SIMULATION_MODE=true` from `.env` and automatically starts
the data generator.  You should see:
```
INFO:     Started server process [12345]
INFO:     Uvicorn running on http://0.0.0.0:8000
INFO:     Simulation mode enabled – generating synthetic data
```

**Terminal 2 – Frontend:**
```bash
cd frontend
npm run dev
```

Open [http://localhost:5173](http://localhost:5173) in your browser.

---

### Running with ESP32 Hardware

1. **Flash the firmware:** See [docs/hardware-integration.md](docs/hardware-integration.md) → Section 6.

2. **Edit firmware constants** (`firmware/esp32_water_controller/esp32_water_controller.ino`):
   ```cpp
   const char* WIFI_SSID      = "YourNetworkName";
   const char* WIFI_PASSWORD  = "YourPassword";
   const char* BACKEND_URL    = "http://192.168.1.100:8000";  // your server IP
   const char* DEVICE_API_KEY = "change-me-device-key-123";   // must match .env
   ```

3. **Set `SIMULATION_MODE=false`** in `.env`:
   ```env
   SIMULATION_MODE=false
   ```

4. **Start backend and frontend** (same as simulation mode above).

5. **Power the ESP32** – it will connect to WiFi and start sending telemetry within 20 s.

6. **Calibrate sensors** – see [docs/calibration.md](docs/calibration.md).

---

## Configuration

All configuration is via environment variables in `.env` (copied from `.env.example`).

| Variable | Default | Description |
|---|---|---|
| `DATABASE_URL` | `sqlite:///./data/water_management.db` | Database connection string |
| `DEVICE_API_KEY` | `change-me-device-key-123` | Secret key for ESP32 authentication |
| `FRONTEND_API_TOKEN` | `change-me-frontend-token-456` | Bearer token for frontend API access |
| `SIMULATION_MODE` | `true` | Generate synthetic data (no ESP32 needed) |
| `CORS_ORIGINS` | `http://localhost:5173` | Allowed CORS origins (comma-separated) |
| `LOG_LEVEL` | `INFO` | Python logging level |
| `VITE_API_URL` | `http://localhost:8000` | Backend URL seen by browser |
| `VITE_WS_URL` | `ws://localhost:8000` | WebSocket URL seen by browser |
| `VITE_FRONTEND_TOKEN` | `change-me-frontend-token-456` | Frontend auth token (must match `FRONTEND_API_TOKEN`) |

> **Security:** Change `DEVICE_API_KEY` and `FRONTEND_API_TOKEN` to strong random
> values before any real deployment. Never commit `.env` to version control.

---

## API Documentation

See [docs/api.md](docs/api.md) for the complete REST + WebSocket reference.

**Quick reference:**

| Endpoint | Auth | Description |
|---|---|---|
| `POST /api/v1/telemetry` | Device key | Receive sensor snapshot from ESP32 |
| `GET /api/v1/status` | Frontend token | Latest device state |
| `GET /api/v1/telemetry/history` | Frontend token | Historical readings |
| `POST /api/v1/commands` | Frontend token | Queue a pump command |
| `GET /api/v1/devices/{device_id}/commands/pending` | Device key | ESP32 fetches pending commands |
| `POST /api/v1/devices/{device_id}/commands/{command_id}/ack` | Device key | ESP32 ACKs command result |
| `GET /api/v1/faults` | Frontend token | Fault history |
| `GET /api/v1/health` | None | Backend health check |
| `WS /ws/dashboard` | Token in query | Real-time telemetry push |

---

## Hardware Setup

See [docs/hardware-integration.md](docs/hardware-integration.md) for:
- Bill of materials
- Wiring diagram
- Pin assignment table
- Flashing instructions
- Per-sensor testing

See [docs/calibration.md](docs/calibration.md) for detailed sensor calibration.

**Supported hardware:**
- ESP32 DevKit (ESP32-WROOM-32, 38-pin or 30-pin)
- HC-SR04 ultrasonic distance sensor
- YF-S201 water flow sensor
- 0–5 bar analog pressure sensor
- ACS712-20A current sensor
- 5 V active-HIGH relay module

---

## Testing

### Backend Unit Tests
```bash
cd backend
source venv/bin/activate
pytest tests/ -v
```

### Frontend Tests
```bash
cd frontend
npm test
```

### Integration Test (end-to-end with simulation)
```bash
# Start backend in simulation mode
cd backend && uvicorn main:app --port 8000 &

# Run integration tests
pytest tests/integration/ -v --base-url http://localhost:8000
```

### Manual Hardware Test
See [docs/hardware-integration.md](docs/hardware-integration.md) → Section 7 for
step-by-step per-sensor testing instructions.

---

## Architecture

See [docs/architecture.md](docs/architecture.md) for:
- ASCII system diagram
- Component descriptions
- Data flow (ESP32 → Backend → WebSocket → Browser)
- Security model
- Offline resilience behaviour

---

## Contributing

1. Fork the repository
2. Create a feature branch: `git checkout -b feature/your-feature`
3. Commit changes: `git commit -m "feat: add your feature"`
4. Push and open a Pull Request

---

## License

MIT License – see `LICENSE` file for details.

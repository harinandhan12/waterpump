# API Reference

## Smart Water Management & Pump Health Monitoring System

**Base URL:** `http://<server-host>:8000`  
**API Version:** v1  
**Content-Type:** `application/json` (all requests and responses)

---

## Authentication

Two separate API keys are used:

| Principal | Header | Value |
|---|---|---|
| ESP32 Device | `X-API-Key` | Value of `DEVICE_API_KEY` in `.env` |
| Frontend / Operator | `Authorization` | `Bearer <FRONTEND_API_TOKEN>` |

Unauthenticated requests receive `HTTP 401 Unauthorized`.

---

## Endpoints

### 1. `POST /api/v1/telemetry`
Receive a sensor snapshot from the ESP32 device.

**Auth:** Device API key (`X-API-Key`)

**Request Body:**
```json
{
  "device_id": "esp32-water-controller-01",
  "timestamp": "2024-01-15T14:32:10Z",
  "tank_distance_cm": 12.5,
  "tank_level_percent": 62.5,
  "flow_rate_lpm": 8.4,
  "pressure_bar": 1.6,
  "pump_current_a": 1.2,
  "pump_on": true,
  "mode": "AUTO",
  "controller_state": "FILLING",
  "fault_code": 0,
  "fault_msg": "",
  "sensor_status": {
    "ultrasonic": "OK",
    "flow": "OK",
    "pressure": "OK",
    "current": "OK"
  }
}
```

**Response `202 Accepted`:**
```json
{
  "status": "accepted",
  "reading_id": 4821
}
```

**Response `422 Unprocessable Entity`:** Malformed or missing required fields.

**Example:**
```bash
curl -X POST http://localhost:8000/api/v1/telemetry \
  -H "Content-Type: application/json" \
  -H "X-API-Key: change-me-device-key-123" \
  -d '{
    "device_id": "esp32-water-controller-01",
    "timestamp": "2024-01-15T14:32:10Z",
    "tank_distance_cm": 15.0,
    "tank_level_percent": 50.0,
    "flow_rate_lpm": 6.0,
    "pressure_bar": 1.4,
    "pump_current_a": 1.1,
    "pump_on": true,
    "mode": "AUTO",
    "controller_state": "FILLING",
    "sensor_status": {
      "ultrasonic": "OK", "flow": "OK", "pressure": "OK", "current": "OK"
    }
  }'
```

---

### 2. `GET /api/v1/status`
Get the latest device state snapshot for the dashboard.

**Auth:** Frontend token (`Authorization: Bearer`)

**Response `200 OK`:**
```json
{
  "device_id": "esp32-water-controller-01",
  "last_seen": "2024-01-15T14:32:10Z",
  "online": true,
  "tank_level_percent": 62.5,
  "flow_rate_lpm": 8.4,
  "pressure_bar": 1.6,
  "pump_current_a": 1.2,
  "pump_on": true,
  "mode": "AUTO",
  "controller_state": "FILLING",
  "fault_code": 0,
  "fault_msg": "",
  "sensor_status": {
    "ultrasonic": "OK",
    "flow": "OK",
    "pressure": "OK",
    "current": "OK"
  }
}
```

**`online`** is `true` if a telemetry reading was received within the last 30 seconds.

**Example:**
```bash
curl http://localhost:8000/api/v1/status \
  -H "Authorization: Bearer change-me-frontend-token-456"
```

---

### 3. `GET /api/v1/telemetry/history`
Retrieve historical telemetry readings.

**Auth:** Frontend token

**Query Parameters:**

| Parameter | Type | Default | Description |
|---|---|---|---|
| `limit` | integer | 100 | Max records to return (max 1000) |
| `offset` | integer | 0 | Pagination offset |
| `since` | ISO8601 string | — | Only return readings after this timestamp |
| `until` | ISO8601 string | — | Only return readings before this timestamp |

**Response `200 OK`:**
```json
{
  "total": 2880,
  "limit": 100,
  "offset": 0,
  "readings": [
    {
      "id": 4821,
      "received_at": "2024-01-15T14:32:10Z",
      "tank_level_percent": 62.5,
      "flow_rate_lpm": 8.4,
      "pressure_bar": 1.6,
      "pump_current_a": 1.2,
      "pump_on": true,
      "controller_state": "FILLING"
    }
  ]
}
```

**Example:**
```bash
curl "http://localhost:8000/api/v1/telemetry/history?limit=50&since=2024-01-15T00:00:00Z" \
  -H "Authorization: Bearer change-me-frontend-token-456"
```

---

### 4. `POST /api/v1/devices/{device_id}/commands`
Queue a command for the ESP32 to execute.

**Auth:** Frontend token

**Request Body:**
```json
{"command_type": "PUMP_ON"}
```

**Supported `command_type` values:**

| Action | Description | Required Mode |
|---|---|---|
| `PUMP_ON` | Turn pump ON | `MANUAL` |
| `PUMP_OFF` | Turn pump OFF | `MANUAL` |
| `SET_MODE_AUTO` | Switch controller to AUTO mode | any |
| `SET_MODE_MANUAL` | Switch controller to MANUAL mode | any |

**Response `201 Created`:**
```json
{
  "command_id": "generated-uuid",
  "device_id": "esp32-water-controller-01",
  "command_type": "PUMP_ON",
  "acknowledged": false
}
```

**Example – switch to manual and turn pump on:**
```bash
# 1. Switch to MANUAL mode
curl -X POST http://localhost:8000/api/v1/devices/esp32-water-controller-01/commands \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer change-me-frontend-token-456" \
  -d '{"command_type": "SET_MODE_MANUAL"}'

# 2. Turn pump ON
curl -X POST http://localhost:8000/api/v1/devices/esp32-water-controller-01/commands \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer change-me-frontend-token-456" \
  -d '{"command_type": "PUMP_ON"}'
```

---

### 5. `GET /api/v1/devices/{device_id}/commands/pending`
Fetch unacknowledged commands for a device. Called by the ESP32.

**Auth:** Device API key

**Response `200 OK`:**
```json
[
  {
    "command_id": "generated-uuid",
    "command_type": "PUMP_ON",
    "acknowledged": false
  }
]
```

Returns `[]` when no commands are pending.

**Example:**
```bash
curl "http://localhost:8000/api/v1/devices/esp32-water-controller-01/commands/pending" \
  -H "X-API-Key: change-me-device-key-123"
```

---

### 6. `POST /api/v1/devices/{device_id}/commands/{command_id}/ack`
Acknowledge command execution result. Called by the ESP32.

**Auth:** Device API key

**Request Body:**
```json
{"device_reported_state": "PUMP_ON"}
```

**Response `200 OK`:**
```json
{"acknowledged": true}
```

**Example:**
```bash
curl -X POST http://localhost:8000/api/v1/devices/esp32-water-controller-01/commands/generated-uuid/ack \
  -H "Content-Type: application/json" \
  -H "X-API-Key: change-me-device-key-123" \
  -d '{"device_reported_state": "PUMP_ON"}'
```

---

### 7. `GET /api/v1/faults`
Retrieve historical fault events.

**Auth:** Frontend token

**Query Parameters:** same as `/api/v1/telemetry/history`

**Response `200 OK`:**
```json
{
  "total": 3,
  "faults": [
    {
      "id": 12,
      "occurred_at": "2024-01-14T09:15:22Z",
      "fault_code": 1,
      "fault_msg": "Flow 0.20 LPM < min 0.50 LPM – possible dry run",
      "controller_state": "FAULT",
      "resolved_at": "2024-01-14T09:32:00Z"
    }
  ]
}
```

**Example:**
```bash
curl http://localhost:8000/api/v1/faults \
  -H "Authorization: Bearer change-me-frontend-token-456"
```

---

### 8. `GET /api/v1/health`
Backend health check (no auth required).

**Response `200 OK`:**
```json
{
  "status": "ok",
  "version": "1.0.0",
  "db": "ok",
  "simulation_mode": false
}
```

**Example:**
```bash
curl http://localhost:8000/api/v1/health
```

---

## WebSocket – Real-Time Dashboard

**Endpoint:** `ws://<server-host>:8000/ws/dashboard`  
**Auth:** Pass the frontend token as a query parameter:  
`ws://localhost:8000/ws/dashboard?token=change-me-frontend-token-456`

### Connection Lifecycle
1. Client connects.
2. Server immediately sends the latest telemetry snapshot.
3. Server pushes a new message on every received telemetry POST (≈every 5 s).
4. Client can send `{"type": "ping"}` keepalive; server replies `{"type": "pong"}`.
5. Server sends `{"type": "disconnected"}` before closing on inactivity (60 s timeout).

### Message Format (server → client)

**Telemetry push:**
```json
{
  "type": "telemetry",
  "data": {
    "device_id": "esp32-water-controller-01",
    "received_at": "2024-01-15T14:32:10Z",
    "tank_level_percent": 62.5,
    "flow_rate_lpm": 8.4,
    "pressure_bar": 1.6,
    "pump_current_a": 1.2,
    "pump_on": true,
    "mode": "AUTO",
    "controller_state": "FILLING",
    "fault_code": 0,
    "fault_msg": "",
    "sensor_status": {
      "ultrasonic": "OK",
      "flow": "OK",
      "pressure": "OK",
      "current": "OK"
    }
  }
}
```

**Fault alert push:**
```json
{
  "type": "fault",
  "data": {
    "fault_code": 1,
    "fault_msg": "Flow 0.20 LPM < min 0.50 LPM – possible dry run",
    "occurred_at": "2024-01-15T14:40:00Z"
  }
}
```

**Ping/Pong:**
```json
{ "type": "ping" }   // client → server
{ "type": "pong" }   // server → client
```

### JavaScript Example
```javascript
const ws = new WebSocket('ws://localhost:8000/ws/dashboard?token=change-me-frontend-token-456');

ws.onopen = () => console.log('Dashboard connected');
ws.onmessage = (event) => {
  const msg = JSON.parse(event.data);
  if (msg.type === 'telemetry') {
    updateDashboard(msg.data);
  } else if (msg.type === 'fault') {
    showFaultAlert(msg.data);
  }
};
ws.onerror = (err) => console.error('WebSocket error:', err);
```

---

## Error Responses

All error responses follow this shape:

```json
{
  "error": "string",
  "detail": "additional context (optional)"
}
```

| HTTP Status | Meaning |
|---|---|
| 400 | Bad Request – malformed JSON or invalid field values |
| 401 | Unauthorized – missing or invalid API key |
| 404 | Not Found – resource does not exist |
| 422 | Unprocessable Entity – schema validation failed |
| 429 | Too Many Requests – rate limit exceeded |
| 500 | Internal Server Error |

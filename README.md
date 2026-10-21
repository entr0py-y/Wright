# Wright — Smart Weighing Machine

> ESP32-powered weighing machine with a live web dashboard. Real-time weight and height measurements displayed on a polished, responsive website.

![Architecture](https://img.shields.io/badge/ESP32-Firmware-blue) ![Backend](https://img.shields.io/badge/Node.js-Express-green) ![Frontend](https://img.shields.io/badge/React-Vite-purple)

---

## Architecture

```
┌─────────────┐       HTTP POST        ┌──────────────┐       HTTP GET       ┌──────────────┐
│   ESP32      │ ──────────────────────►│   Backend    │ ◄────────────────── │   Frontend   │
│   Firmware   │   /api/readings        │   (Express)  │  /api/readings/     │   (React)    │
│              │   + X-API-Key header   │   Port 3001  │  latest + history   │   Port 5173  │
├──────────────┤                        ├──────────────┤                     ├──────────────┤
│ HX711 Weight │                        │ In-memory    │                     │ Dashboard    │
│ HC-SR04 Ht   │                        │ storage      │                     │ Live charts  │
│ Wi-Fi        │                        │ Validation   │                     │ Status panel │
└─────────────┘                        │ Auth (API key)│                     └──────────────┘
                                       └──────────────┘
```

## Hardware (DO NOT MODIFY)

| Component | GPIO | Notes |
|-----------|------|-------|
| HX711 DOUT | GPIO 19 | Data output |
| HX711 SCK | GPIO 18 | Serial clock |
| HC-SR04 TRIG | GPIO 25 | Trigger pulse |
| HC-SR04 ECHO | GPIO 26 | With voltage divider |

- **4× three-wire load cells** → Load Combinator board → HX711
- **Weight calibration factor**: 16233.3 (configurable in `firmware/config.h`)
- **Sensor mount height**: ~7 feet (213.36 cm)

---

## Quick Start

### Prerequisites

- [Node.js](https://nodejs.org/) ≥ 18
- [Arduino IDE](https://www.arduino.cc/en/software) ≥ 2.x (for ESP32 firmware)
- ESP32 board support installed in Arduino IDE

### 1. Clone & Configure

```bash
git clone <your-repo-url> Wright
cd Wright

# Copy and edit environment files
cp .env.example backend/.env
# Edit backend/.env with your API_KEY
```

### 2. Start the Backend

```bash
cd backend
npm install
npm run dev
```

The API starts on `http://localhost:3001`. Verify:

```bash
curl http://localhost:3001/api/health
```

### 3. Start the Frontend

```bash
cd frontend
npm install
npm run dev
```

Dashboard opens at `http://localhost:5173`.

### 4. Upload ESP32 Firmware

1. Open `firmware/firmware.ino` in Arduino IDE.
2. Install required libraries via Library Manager:
   - **HX711** by bogde (v0.7.5+)
   - **ArduinoJson** by Benoit Blanchon (v7.x)
   - (The following libraries are built into the ESP32 board package: `WiFi`, `WebServer`, `DNSServer`, `HTTPClient`, `Preferences`)
3. Edit `firmware/config.h`:
   ```cpp
   #define BACKEND_URL     "http://YOUR_COMPUTER_IP:3001"
   #define API_KEY         "same-key-as-backend-env"
   #define AP_PASSWORD     "WrightScale2026!"  // Setup hotspot password (min 8 chars)
   ```
   *(Optional)* Set `WIFI_SSID_DEFAULT` / `WIFI_PASSWORD_DEFAULT` if you want hardcoded initial credentials, or leave as `"YOUR_WIFI_SSID"` to automatically launch the Wi-Fi Setup Hotspot on first boot!
4. Select board: **ESP32 Dev Module**
5. Upload and open Serial Monitor (115200 baud).

---

## 📶 Automatic Wi-Fi Recovery & Configuration Portal

The ESP32 firmware features an automatic Wi-Fi recovery and setup portal:

```
┌────────────────────────────────────────────────────────┐
│               ESP32 OPERATING MODES                    │
├───────────────────────────┬────────────────────────────┤
│ Mode 1: Normal Connected  │ Mode 2: Setup Hotspot      │
├───────────────────────────┼────────────────────────────┤
│ • Connected to saved Wi-Fi│ • Broadcasts Setup Hotspot │
│ • Streams live readings   │ • Captive portal web server│
│ • Web dashboard updates   │ • Scan & enter Wi-Fi creds │
│ • Auto-reconnects on drop │ • Validates before saving  │
│ • Falls back to Portal    │ • Persists in flash (NVS)  │
│   if unreachable for 60s  │ • Never reboots/crashes    │
└───────────────────────────┴────────────────────────────┘
```

### How to Configure Wi-Fi via Portal

1. **Triggering the Hotspot**:
   - On first boot (no credentials saved) or when your Wi-Fi is unreachable for 60s, the ESP32 automatically enters **Portal Mode**.
   - The device creates a secure Wi-Fi access point:
     - **SSID**: `SmartScale-Setup` (configured via `AP_SSID`)
     - **Password**: `WrightScale2026!` (configured via `AP_PASSWORD`, min 8 characters)
     - **IP**: `192.168.4.1`

2. **Connecting to the Portal**:
   - Connect your phone, tablet, or laptop to the `SmartScale-Setup` Wi-Fi network.
   - A captive portal notification ("Sign in to network") will appear automatically on iOS, Android, macOS, and Windows.
   - If it does not open automatically, open your browser and navigate to:
     ```
     http://192.168.4.1
     ```

3. **Scanning and Saving Credentials**:
   - Tap **Scan for Networks** to view nearby 2.4 GHz Wi-Fi networks sorted by signal strength.
   - Tap your network to auto-fill the SSID.
   - Enter your Wi-Fi password and tap **Save & Connect**.

4. **Live Verification & Rollback Protection**:
   - The ESP32 enters dual `WIFI_AP_STA` mode — the hotspot **stays alive** while the scale tests connecting to your router.
   - A live progress indicator updates you in real time:
     - **Success**: Credentials are saved to non-volatile flash (`Preferences`), the user is shown the assigned IP, and the hotspot closes gracefully after 3.5 seconds. The scale switches to Normal Connected Mode.
     - **Failure** (wrong password / out of range): The hotspot remains open, a clear error message is displayed, and previous credentials (if any) are retained. You can retry immediately without having to reconnect to the hotspot.

5. **Resetting Credentials**:
   - You can clear all saved credentials at any time by tapping **Reset Saved Credentials** at the bottom of the portal page.

---

## Network Deployment Modes

### Mode A: Local Network (Development)

Both ESP32 and backend are on the same Wi-Fi network.

1. Find your computer's LAN IP:
   ```bash
   # macOS
   ipconfig getifaddr en0
   # Linux
   hostname -I
   # Windows
   ipconfig
   ```

2. Set `BACKEND_URL` in `firmware/config.h`:
   ```cpp
   #define BACKEND_URL "http://192.168.1.100:3001"
   ```

3. **Firewall**: Ensure port 3001 is accessible:
   - **macOS**: System Preferences → Security → Firewall → allow Node.js
   - **Linux**: `sudo ufw allow 3001/tcp`
   - **Windows**: Windows Defender Firewall → Inbound Rule → Port 3001

### Mode B: Internet-Hosted (Production)

#### Deploy Backend to Render (free tier)

1. Push `backend/` to a Git repository.
2. Create a new **Web Service** on [Render](https://render.com):
   - **Build Command**: `npm install`
   - **Start Command**: `npm start`
   - **Environment Variables**:
     - `API_KEY` = your secret key
     - `CORS_ORIGIN` = your frontend URL (e.g., `https://wright.vercel.app`)
     - `NODE_ENV` = `production`
3. Note the URL (e.g., `https://wright-api.onrender.com`).

> ⚠️ **Render free tier**: Service sleeps after 15 minutes of inactivity. First request after sleep takes ~30 seconds. Consider the Starter plan ($7/mo) for always-on.

#### Deploy Frontend to Vercel (free tier)

1. Push `frontend/` to a Git repository.
2. Import in [Vercel](https://vercel.com):
   - **Framework**: Vite
   - **Build Command**: `npm run build`
   - **Output Directory**: `dist`
   - **Environment Variable**: `VITE_API_URL` = your Render backend URL

#### Update ESP32

```cpp
#define BACKEND_URL "https://wright-api.onrender.com"
#define API_KEY     "your-production-secret-key"
```

---

## API Reference

### `GET /api/health`

Returns server status.

```json
{
  "status": "ok",
  "uptime": 3600.5,
  "startedAt": "2026-10-21T06:00:00.000Z",
  "totalReadings": 3500,
  "hasLatestReading": true,
  "timestamp": "2026-10-21T07:00:00.500Z"
}
```

### `POST /api/readings`

Receive a sensor reading from ESP32.

**Headers:**
```
Content-Type: application/json
X-API-Key: your-secret-key
```

**Request Body:**
```json
{
  "deviceId": "wright-esp32-01",
  "weightKg": "5.250",
  "heightCm": "15.30",
  "timestamp": "00:01:23",
  "status": "ok"
}
```

**Response (201):**
```json
{
  "message": "Reading received",
  "serverTimestamp": "2026-10-21T06:38:59.517Z"
}
```

**Response (401 - Missing/Invalid API Key):**
```json
{
  "error": "Unauthorized",
  "message": "Missing or invalid API key. Include X-API-Key header."
}
```

**Response (400 - Validation Error):**
```json
{
  "error": "Validation failed",
  "details": [
    "weightKg must be a number between -1 and 500"
  ]
}
```

### `GET /api/readings/latest`

Returns the most recent valid reading.

```json
{
  "deviceId": "wright-esp32-01",
  "weightKg": 5.25,
  "heightCm": 15.3,
  "status": "ok",
  "deviceTimestamp": "00:01:23",
  "serverTimestamp": "2026-10-21T06:38:59.517Z",
  "receivedAt": 1792564739517,
  "ageMs": 1200,
  "isStale": false,
  "isOnline": true
}
```

### `GET /api/readings/history?limit=50`

Returns recent readings (max 100).

```json
{
  "count": 50,
  "readings": [...]
}
```

---

## Configuration Reference

### `firmware/config.h`

| Setting | Default | Description |
|---------|---------|-------------|
| `WIFI_SSID_DEFAULT` | `"YOUR_WIFI_SSID"` | Initial fallback SSID (or leave placeholder for portal on boot) |
| `WIFI_PASSWORD_DEFAULT` | `"YOUR_WIFI_PASSWORD"` | Initial fallback password |
| `AP_SSID` | `"SmartScale-Setup"` | Setup hotspot network name |
| `AP_PASSWORD` | `"WrightScale2026!"` | Hotspot WPA2 password (≥8 chars) |
| `AP_IP` | `192.168.4.1` | Portal web server IP address |
| `WIFI_CONNECT_TIMEOUT_MS` | `30000` | Boot connection timeout before opening portal (30s) |
| `WIFI_PORTAL_TRIGGER_MS` | `60000` | Reconnect failure timeout before opening portal (60s) |
| `PORTAL_CONNECT_ATTEMPT_MS` | `15000` | Max time to validate new credentials before rollback (15s) |
| `BACKEND_URL` | `"http://192.168.1.100:3001"` | Backend API URL |
| `API_KEY` | `"change-me..."` | Must match backend's API_KEY |
| `DEVICE_ID` | `"wright-esp32-01"` | Unique device identifier |
| `CALIBRATION_FACTOR` | `16233.3` | HX711 calibration factor |
| `SENSOR_MOUNT_HEIGHT_CM` | `243.84` | Ultrasonic sensor height above platform (8 feet) |
| `ENABLE_TEST_RANGE` | `true` | Restrict max measured height |
| `TEST_MAX_HEIGHT_CM` | `243.84` | Max height in test mode (8 feet) |
| `SEND_INTERVAL_MS` | `1000` | Transmission interval |

### `backend/.env`

| Variable | Default | Description |
|----------|---------|-------------|
| `PORT` | `3001` | Server port |
| `API_KEY` | `change-me...` | Authentication key |
| `CORS_ORIGIN` | `http://localhost:5173` | Allowed frontend origin(s) |
| `NODE_ENV` | `development` | Environment mode |

### `frontend` (Vite env)

| Variable | Default | Description |
|----------|---------|-------------|
| `VITE_API_URL` | (empty = proxy) | Backend URL for production |

---

## Status Codes from ESP32

| Status | Meaning |
|--------|---------|
| `ok` | Both sensors reading normally |
| `idle` | Weight = 0, height = 0 (nothing on platform) |
| `error_weight_sensor` | HX711 not responding |
| `error_height_sensor` | HC-SR04 no echo / invalid reading |
| `error_both_sensors` | Both sensors failing |
| `height_out_of_range` | Height exceeds test range limit |

---

## Troubleshooting

### Wi-Fi Connection

| Symptom | Fix |
|---------|-----|
| Serial shows `...` then "Initial connection failed" | Verify SSID/password in `config.h`. Check ESP32 is within Wi-Fi range. |
| Connects then immediately disconnects | Check Wi-Fi router for MAC filtering. Reduce distance. |
| `[WIFI] Reconnecting...` in loop | Network may be overloaded. Increase `WIFI_RECONNECT_INTERVAL_MS`. |

### Wi-Fi Configuration Portal

| Symptom | Fix |
|---------|-----|
| `SmartScale-Setup` network not appearing | Allow up to 30s after boot or 60s after Wi-Fi loss. Check Serial Monitor for `[PORTAL] AP SSID: SmartScale-Setup`. |
| Cannot join `SmartScale-Setup` Wi-Fi | Verify you are entering the password in `AP_PASSWORD` (default: `WrightScale2026!`). Must be ≥8 chars. |
| Portal page does not open automatically | Open your browser and navigate directly to `http://192.168.4.1`. |
| "Failed to connect" after entering credentials | Double check your Wi-Fi password. Note that ESP32 only connects to 2.4 GHz Wi-Fi networks (not 5 GHz-only SSIDs). |
| Portal stuck on "Connecting..." | Ensure the router is powered on and within range. The portal times out safely after 15s (`PORTAL_CONNECT_ATTEMPT_MS`). |
| Want to change Wi-Fi network after it's connected | Turn off your router or move out of range for 60s, or use the portal's **Reset Saved Credentials** button. |

### API / Network

| Symptom | Fix |
|---------|-----|
| `[HTTP] Request failed: connection refused` | Backend not running or wrong IP. Check `BACKEND_URL`. Run `curl http://IP:3001/api/health` from another device. |
| `[HTTP] Server error (401)` | API keys don't match. Check `API_KEY` in both `config.h` and `.env`. |
| `[HTTP] Server error (400)` | Data validation failed. Check Serial output for sensor errors. |
| Dashboard says "Backend connection error" | Backend not running, or CORS misconfigured. Check `CORS_ORIGIN` in `.env`. |
| Firewall blocking | Allow port 3001 in your OS firewall settings. |

### Stale / Missing Readings

| Symptom | Fix |
|---------|-----|
| Dashboard says "Waiting for ESP32 data" | ESP32 not sending. Check Serial Monitor for HTTP errors. |
| "Data Stale" badge appears | ESP32 stopped sending >10s ago. Check Wi-Fi and power supply. |
| Weight shows 0 after power cycle | Normal — scale tares at startup. Ensure platform is empty during boot. |
| Weight drifts over time | Temperature drift is normal for load cells. Re-tare or recalibrate. |

### Sensor Issues

| Symptom | Fix |
|---------|-----|
| Weight always -1 | HX711 not detected. Check GPIO 18/19 wiring and power. |
| Height always -1 | HC-SR04 no echo. Check GPIO 25/26, voltage divider, and clear line of sight. |
| "Out of Range" for height | Test range is enabled (30.48 cm max). Set `ENABLE_TEST_RANGE false` or increase `TEST_MAX_HEIGHT_CM`. |
| Inconsistent weight readings | Increase `WEIGHT_SAMPLES` in `config.h`. Ensure load cells are seated flat. |

### Deployment

| Symptom | Fix |
|---------|-----|
| Render service sleeping | Free tier sleeps after 15 min. Use a ping service like UptimeRobot, or upgrade. |
| CORS errors in browser console | Set `CORS_ORIGIN` in backend `.env` to match your frontend URL exactly. |
| ESP32 HTTPS fails | ESP32 HTTPClient needs WiFiClientSecure for HTTPS. See note below. |

> **HTTPS Note**: The firmware uses `HTTPClient` which works with HTTP. For HTTPS (internet deployment), you may need to add `WiFiClientSecure` with the server's root CA certificate, or use `setInsecure()` for testing. This is documented in the Arduino ESP32 examples.

---

## Project Structure

```
Wright/
├── firmware/
│   ├── firmware.ino          # ESP32 main firmware
│   └── config.h              # All configurable settings
├── backend/
│   ├── src/
│   │   └── server.js         # Express API server
│   ├── package.json
│   ├── .env.example
│   └── .env                  # Local secrets (git-ignored)
├── frontend/
│   ├── src/
│   │   ├── main.jsx          # React entry point
│   │   ├── App.jsx           # Dashboard component
│   │   ├── App.css           # Component styles
│   │   ├── index.css          # Global design system
│   │   └── useWrightData.js  # API polling hook
│   ├── public/
│   │   └── favicon.svg
│   ├── index.html
│   ├── vite.config.js
│   └── package.json
├── .env.example              # Root env template
├── .gitignore
└── README.md                 # This file
```

---

## License

MIT

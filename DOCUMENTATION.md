# Wright — Smart Weighing Machine Documentation

> **Complete Technical Architecture, Pinout Wiring, Deployment Pipeline, Calibration Model, and Operations Guide.**

---

## 1. System Pipeline & Architecture

Wright is a complete IoT hardware-to-cloud telemetry system that captures real-time biometric readings (weight and stature) and streams them continuously to a responsive dashboard.

```
┌─────────────────────────────────────────────────────────────┐
│                    HARDWARE SENSING                         │
│                                                             │
│   [4× 50kg Half-Bridge Load Cells]                          │
│                 │ (Shear force)                             │
│                 ▼                                           │
│       [HX711 24-bit ADC] ──► GPIO 18/19                     │
│                                      ──► [ESP32-WROOM-32D]  │
│       [HC-SR04 Ultrasonic] ─► GPIO 25/26                    │
│                 │ (Time-of-Flight)                          │
└─────────────────┼───────────────────────────────────────────┘
                  │
                  │ WiFi (HTTPS JSON POST / 1 Hz)
                  ▼
┌─────────────────────────────────────────────────────────────┐
│                 CLOUD BACKEND (Render)                      │
│        https://wright-y01b.onrender.com                     │
│                                                             │
│   • Node.js / Express Engine                                │
│   • Multi-Point Nonlinear Spline Interpolation Engine       │
│   • Live Telemetry & Health Endpoints                       │
│   • Permissive CORS Policy for Web Clients                  │
└─────────────────┬───────────────────────────────────────────┘
                  │
                  │ HTTPS JSON GET Polling (1 Hz)
                  ▼
┌─────────────────────────────────────────────────────────────┐
│                 WEB DASHBOARD (Render)                      │
│        https://wright-1-9x3e.onrender.com                   │
│                                                             │
│   • React 19 + Vite Industrial Duo-Color UI                 │
│   • Real-Time Digital Scale Display                         │
│   • Live Raw ADC Counts & Tare Telemetry                    │
│   • Temporal Signal Waveform Graph                          │
└─────────────────────────────────────────────────────────────┘
```

---

## 2. Hardware Pinout & Wiring Specification

> [!IMPORTANT]
> The GPIO assignments are locked in hardware. Do not reassign these pins without modifying circuit traces.

### ESP32-WROOM Pinout Table

| Peripheral | ESP32 Pin | GPIO | Wire Color / Spec | Function |
| :--- | :--- | :--- | :--- | :--- |
| **HX711 DOUT** | Pin 31 | **GPIO 19** | Data (Yellow/White) | Serial 24-bit ADC Output |
| **HX711 SCK** | Pin 30 | **GPIO 18** | Clock (Orange/Yellow) | Serial Clock Pulse Input |
| **HX711 VCC** | 5V / VIN | — | Red | 5V Clean DC Power |
| **HX711 GND** | GND | — | Black | Common Ground |
| **HC-SR04 TRIG**| Pin 20 | **GPIO 25** | Blue / Green | 10µs Trigger Pulse (Output) |
| **HC-SR04 ECHO**| Pin 11 | **GPIO 26** | White / Yellow | Echo Pulse (Input via Voltage Divider) |
| **HC-SR04 VCC** | 5V / VIN | — | Red | 5V Clean DC Power |
| **HC-SR04 GND** | GND | — | Black | Common Ground |

### HC-SR04 Echo Voltage Divider Notice:
The HC-SR04 operates at 5V logic. ESP32 GPIOs are 3.3V tolerant only:
- **HC-SR04 ECHO Pin** $\longrightarrow$ **$1\text{k}\Omega$ Resistor** $\longrightarrow$ **GPIO 26**
- **GPIO 26** $\longrightarrow$ **$2\text{k}\Omega$ Resistor** $\longrightarrow$ **GND**
*(Steps 5V echo pulse down safely to $\approx 3.33\text{V}$)*.

### Ultrasonic Mounting Height:
- **Mount Height**: `243.84 cm` (8.0 feet from platform surface).
- **Calculation Formula**: $\text{Height (cm)} = 243.84\text{ cm} - \text{Measured Distance (cm)}$.

---

## 3. Live Cloud Services & URLs

| Service | Host | Live Production URL | Purpose |
| :--- | :--- | :--- | :--- |
| **Web Dashboard** | Render (Static Site) | **`https://wright-1-9x3e.onrender.com`** | Primary User Interface |
| **Backend API** | Render (Web Service) | **`https://wright-y01b.onrender.com`** | Central Ingestion Engine |
| **Health Check** | Render API | `https://wright-y01b.onrender.com/api/health` | Server Heartbeat |
| **Latest Telemetry** | Render API | `https://wright-y01b.onrender.com/api/readings/latest` | JSON Feed for Client |
| **GitHub Repo** | GitHub | `https://github.com/entr0py-y/Wright` | Source Code Control |
| **Local Dashboard**| Localhost | `http://localhost:5173` | Local Development UI |
| **Local Backend** | Localhost | `http://localhost:3001` | Local Development API |

---

## 4. How to Change Wi-Fi Credentials (Zero Code Re-Flashing)

You **never** need to re-flash the ESP32 code to change Wi-Fi networks. The firmware features an integrated On-Demand Captive Portal.

### Step-by-Step Wi-Fi Reconfiguration:

1. **Triggering Portal Mode**:
   - Turn off your previous phone hotspot or move to the new location.
   - Power on the ESP32.
   - When the ESP32 cannot connect to its saved Wi-Fi for 30 seconds, it automatically launches its own setup hotspot.

2. **Connect to Setup Hotspot**:
   - On your phone or laptop, open your Wi-Fi settings.
   - Connect to:
     - **Network (SSID)**: `SmartScale-Setup`
     - **Password**: `WrightScale2026!`

3. **Open the Configuration Console**:
   - Open any browser (Safari, Chrome, Brave).
   - Navigate to: **`http://192.168.4.1`**
   *(Most devices will also show a pop-up saying "Log in to network")*.

4. **Scan & Submit**:
   - Tap **[ SCAN FOR 2.4GHZ NETWORKS ]**.
   - Tap your desired Wi-Fi network from the list.
   - Type your Wi-Fi password.
   - Tap **SAVE & VERIFY CONNECTION**.

5. **Automatic Verification**:
   - The ESP32 tests the credentials against your router while keeping the setup hotspot active.
   - Once verified, it writes the credentials to internal Non-Volatile Flash (`Preferences`/NVS), shuts down the setup hotspot, and immediately begins streaming weight and stature to `https://wright-y01b.onrender.com`.

---

## 5. Weight Calibration Model & Physics

### Why Standard Single-Point Calibration Fails:
Four 50kg half-bridge load cells (200kg total mechanical rating) behave with structural mechanical hysteresis:
- **Light loads (< 10 kg)**: The platform does not fully compress against the corner rubber mounts, creating an initial shallow sensitivity slope ($\approx 15,500\text{ counts/kg}$).
- **Human bodyweights (> 35 kg)**: All four load cells fully deflect into linear shear range, producing a steeper slope ($\approx 26,860\text{ counts/kg}$).

### The Calibrated Multi-Point Spline Model:
Both the backend (`backend/src/server.js`) and frontend (`frontend/src/App.jsx`) utilize exact piecewise linear interpolation derived from real physical weights:

$$\text{Net ADC Counts} = \text{Raw ADC} - \text{Zero Tare Offset}$$

| Net ADC Counts Range | Physical Weight Range | Interpolation Formula |
| :--- | :--- | :--- |
| $\text{Net} \le 34,181$ | **0 kg to 2.20 kg** | $\text{kg} = \frac{\text{Net}}{34,181} \times 2.20$ |
| $34,181 < \text{Net} \le 92,810$ | **2.20 kg to 4.50 kg** | $\text{kg} = 2.20 + \frac{\text{Net} - 34,181}{58,629} \times 2.30$ |
| $92,810 < \text{Net} \le 146,901$ | **4.50 kg to 6.00 kg** | $\text{kg} = 4.50 + \frac{\text{Net} - 92,810}{54,091} \times 1.50$ |
| $\text{Net} > 146,901$ | **6.00 kg to 150 kg+** | $\text{kg} = 6.00 + \frac{\text{Net} - 146,901}{26,860.0}$ |

> [!TIP]
> This software calibration completely frees the hardware. If you ever need to adjust weights, you modify 4 numbers in `backend/src/server.js` and push to GitHub. You never have to touch the ESP32 firmware.

---

## 6. Repository Structure

```
Wright/
├── firmware/
│   ├── firmware.ino          # Complete standalone ESP32 firmware
│   └── config.h              # Header with pinouts and default configuration
├── backend/
│   ├── src/
│   │   └── server.js         # Node/Express API, calibration engine, CORS
│   ├── package.json          # Express, cors, express-rate-limit
│   └── .env.example          # Environment variables template
├── frontend/
│   ├── src/
│   │   ├── App.jsx           # Minimal duo-color industrial dashboard
│   │   ├── App.css           # Monospaced layout and typography
│   │   ├── index.css         # Reset and global variables (#000000 / #ffffff)
│   │   └── useWrightData.js  # API polling hook with Render fallback
│   ├── vite.config.js        # Vite dev server and proxy
│   └── package.json          # React 19, Vite
└── README.md                 # Project README
```

---

## 7. Local Development Guide

### Running Backend Locally:
```bash
cd backend
npm install
npm run dev
# Starts on http://localhost:3001
```

### Running Frontend Locally:
```bash
cd frontend
npm install
npm run dev
# Starts on http://localhost:5173
```

### API Endpoints Reference:
- `POST /api/readings`: Receives sensor packets from ESP32 (`X-API-Key` required).
- `GET /api/readings/latest`: Returns latest cached reading and online status.
- `GET /api/readings/history?limit=30`: Returns historical time-series for chart.
- `GET /api/health`: Returns server uptime and reading counters.

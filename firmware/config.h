/*
 * Wright — Smart Weighing Machine ESP32 Firmware
 * ================================================
 * Configuration file — edit these values for your setup.
 * This file is the ONLY place you need to change settings.
 */

#ifndef CONFIG_H
#define CONFIG_H

// ─── Wi-Fi Credentials (Fallback / Initial) ────────────
// These are used ONLY on first boot or after a credential reset.
// After the first successful connection, credentials are stored
// in non-volatile flash (Preferences) and updated via the portal.
// Set to empty strings to force portal mode on first boot.
#define WIFI_SSID_DEFAULT       "Heisenberg69"
#define WIFI_PASSWORD_DEFAULT   "11111111"

// Backwards-compatible aliases
#ifndef WIFI_SSID
#define WIFI_SSID               WIFI_SSID_DEFAULT
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD           WIFI_PASSWORD_DEFAULT
#endif

// ─── Wi-Fi Configuration Portal (AP Mode) ──────────────
// SSID for the setup hotspot (visible when portal is active)
#define AP_SSID             "SmartScale-Setup"

// Password for the setup hotspot — CHANGE THIS before deploying.
// Must be ≥8 characters for WPA2.  Do NOT commit a real password.
#define AP_PASSWORD         "WrightScale2026!"

// IP address of the configuration portal (standard ESP32 AP IP)
// Users connect to this address in their browser.
#define AP_IP               IPAddress(192, 168, 4, 1)

// ─── Wi-Fi Timeouts ────────────────────────────────────
// How long to try connecting to saved Wi-Fi before opening portal (ms)
// Default: 30 seconds
#define WIFI_CONNECT_TIMEOUT_MS     30000

// How long to wait between reconnect attempts in normal mode (ms)
#define WIFI_RECONNECT_INTERVAL_MS  5000

// After reconnect failures for this long, switch to portal mode (ms)
// Default: 60 seconds of continuous failure
#define WIFI_PORTAL_TRIGGER_MS      60000

// How long the portal stays active without a connection attempt
// before retrying saved credentials (ms).  0 = stay open indefinitely.
#define PORTAL_IDLE_TIMEOUT_MS      0

// Maximum time to wait while validating new credentials (ms)
#define PORTAL_CONNECT_ATTEMPT_MS   15000

// ─── Backend API ─────────────────────────────────────────
// Mode A (local LAN / Hotspot): "http://10.21.107.106:3001"
// Mode B (internet):  "https://your-app.onrender.com"
#define BACKEND_URL     "https://wright-y01b.onrender.com"
#define API_KEY         "change-me-to-a-long-random-string"

// ─── Device Identity ────────────────────────────────────
#define DEVICE_ID       "wright-esp32-01"

// ─── HX711 Load Cell Configuration ─────────────────────
// GPIO pins — DO NOT CHANGE (matches existing wiring)
#define HX711_DOUT_PIN  19
#define HX711_SCK_PIN   18

// Provisional calibration factor — adjust after calibration
#define CALIBRATION_FACTOR  20419.0f

// Number of readings to average for stable weight
#define WEIGHT_SAMPLES  5

// Minimum weight threshold in kg (below this = 0)
#define WEIGHT_THRESHOLD  0.05f

// ─── HC-SR04 Ultrasonic Configuration ───────────────────
// GPIO pins — DO NOT CHANGE (matches existing wiring with voltage divider on ECHO)
#define TRIG_PIN        25
#define ECHO_PIN        26

// Physical sensor mounting height above the weighing platform (cm)
// Set to 8 feet (8 × 30.48 cm = 243.84 cm)
#define SENSOR_MOUNT_HEIGHT_CM  243.84f

// Maximum distance the HC-SR04 can reliably measure (cm)
#define MAX_DISTANCE_CM  400.0f

// Speed of sound in cm/µs (at ~20°C)
#define SOUND_SPEED_CM_PER_US  0.0343f

// ─── Height Test Range ──────────────────────────────────
// Enable to restrict maximum measured height (set to 8 feet)
#define ENABLE_TEST_RANGE       true
#define TEST_MAX_HEIGHT_CM      243.84f  // 8 feet (243.84 cm)

// ─── Timing ─────────────────────────────────────────────
// Interval between measurement transmissions (ms)
#define SEND_INTERVAL_MS        1000

// HTTP request timeout (ms)
#define HTTP_TIMEOUT_MS         5000

// ─── Serial Debug ───────────────────────────────────────
#define SERIAL_BAUD_RATE  115200

// ─── NVS Storage Namespace ──────────────────────────────
// Used by Preferences library to persist Wi-Fi credentials.
#define NVS_NAMESPACE   "wright"
#define NVS_KEY_SSID    "wifi_ssid"
#define NVS_KEY_PASS    "wifi_pass"
#define NVS_KEY_SAVED   "wifi_saved"  // bool flag: true if creds exist

#endif // CONFIG_H

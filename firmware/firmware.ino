/*
 * Wright — Smart Weighing Machine ESP32 Firmware
 * ================================================
 *
 * Reads weight from HX711 (GPIO 18/19) and height from HC-SR04 (GPIO 25/26),
 * connects directly to the configured Wi-Fi network, and sends JSON readings
 * to the backend API over HTTP(S).
 *
 * Hardware (DO NOT CHANGE):
 *   - HX711:   DOUT=GPIO19, SCK=GPIO18
 *   - HC-SR04: TRIG=GPIO25, ECHO=GPIO26 (with voltage divider)
 *   - Four 3-wire load cells → Load Combinator → HX711
 *
 * Required Libraries:
 *   - HX711 by bogde (v0.7.5+)
 *   - WiFi       (built-in for ESP32)
 *   - HTTPClient (built-in for ESP32)
 *
 * Board: ESP32 Dev Module (ESP32-WROOM)
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include "HX711.h"

// ═════════════════════════════════════════════════════════
// CONFIGURATION SETTINGS
// ═════════════════════════════════════════════════════════
// Wi-Fi Credentials
#define WIFI_SSID                   "Heisenberg69"
#define WIFI_PASSWORD               "11111111"
#define WIFI_CONNECT_TIMEOUT_MS     20000
#define WIFI_RECONNECT_INTERVAL_MS  15000

// Backend API
#define BACKEND_URL     "https://wright-y01b.onrender.com"
#define API_KEY         "change-me-to-a-long-random-string"
#define DEVICE_ID       "wright-esp32-01"

// HX711 Load Cell (Weight)
#define HX711_DOUT_PIN      19
#define HX711_SCK_PIN       18
#define CALIBRATION_FACTOR  26818.5f
#define WEIGHT_SAMPLES      5
// Minimum weight threshold in kg (below this = 0)
#define WEIGHT_THRESHOLD        0.05f

// Auto-zero negative drift when consecutive unloaded samples change by less than this (kg)
#define IDLE_DRIFT_MAX_STEP_KG  0.30f
#define IDLE_REZERO_SAMPLES     3

// HC-SR04 Ultrasonic (Height)
#define TRIG_PIN                25
#define ECHO_PIN                26
#define SENSOR_MOUNT_HEIGHT_CM  243.84f  // 8 feet (243.84 cm)
#define MAX_DISTANCE_CM         400.0f
#define SOUND_SPEED_CM_PER_US   0.0343f
#define ENABLE_TEST_RANGE       true
#define TEST_MAX_HEIGHT_CM      243.84f  // 8 feet (243.84 cm)

// Timing & Serial
#define SEND_INTERVAL_MS        1000
#define HTTP_TIMEOUT_MS         5000
#define SERIAL_BAUD_RATE        115200

// ─── Global Objects & State ─────────────────────────────
HX711 scale;

unsigned long lastSendTime = 0;
unsigned long lastWifiAttempt = 0;
bool wasWifiConnected = false;
bool scaleReady = false;
long lastRawReading = 0;
bool hasPrevRaw = false;
int idleStableCount = 0;
long lastEchoDurationUs = 0;
float lastUltrasonicDistCm = -1.0f;
int consecutiveHttpFailures = 0;

// ─── Function Prototypes ────────────────────────────────
void setupWiFi();
const char* wifiStatusText(wl_status_t status);
void setupScale();
bool waitForScaleReady(unsigned long timeoutMs);
void setupUltrasonic();
long readRawWeight();
float readWeight();
float readDistanceCm();
float readHeightCm();
void sendReading(float weightKg, float heightCm);
String getTimestamp();
String getStatus(float weightKg, float heightCm);

// ═════════════════════════════════════════════════════════
// SETUP
// ═════════════════════════════════════════════════════════
void setup() {
  Serial.begin(SERIAL_BAUD_RATE);
  delay(1000);

  Serial.println();
  Serial.println("╔══════════════════════════════════════════╗");
  Serial.println("║   Wright — Smart Weighing Machine        ║");
  Serial.println("║   ESP32 Firmware (Direct Wi-Fi)          ║");
  Serial.println("╚══════════════════════════════════════════╝");
  Serial.println();

  // Start Wi-Fi first so RF power draw stabilizes before taring the scale
  setupWiFi();
  setupScale();
  setupUltrasonic();

  Serial.println();
  Serial.println("[READY] System initialized. Starting measurement loop...");
  Serial.println();
}

// ═════════════════════════════════════════════════════════
// MAIN LOOP
// ═════════════════════════════════════════════════════════
void loop() {
  unsigned long now = millis();

  // Monitor Wi-Fi and reconnect cleanly without aborting active handshakes
  wl_status_t wifiStatus = WiFi.status();
  if (wifiStatus == WL_CONNECTED) {
    if (!wasWifiConnected) {
      wasWifiConnected = true;
      Serial.printf("[WIFI] Connected! IP: %s | Signal: %d dBm\n",
                    WiFi.localIP().toString().c_str(), WiFi.RSSI());
    }
  } else {
    if (wasWifiConnected) {
      wasWifiConnected = false;
      lastWifiAttempt = now;
      Serial.printf("[WIFI] Connection lost (%s) — waiting for auto-reconnect...\n",
                    wifiStatusText(wifiStatus));
    } else if (now - lastWifiAttempt >= WIFI_RECONNECT_INTERVAL_MS) {
      lastWifiAttempt = now;
      Serial.printf("[WIFI] Still disconnected (%s) — retrying '%s'...\n",
                    wifiStatusText(wifiStatus), WIFI_SSID);
      WiFi.disconnect(false);
      WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    }
  }

  // Read sensors and transmit at SEND_INTERVAL_MS
  if (now - lastSendTime >= SEND_INTERVAL_MS) {
    lastSendTime = now;

    float weightKg = readWeight();
    float heightCm = readHeightCm();

    Serial.printf("[SENSOR] Weight: %.3f kg (Raw: %ld, Offset: %ld) | Height: %.2f cm",
                  weightKg, lastRawReading, (scaleReady ? scale.get_offset() : 0), heightCm);
    if (heightCm > 0) {
      Serial.printf(" (%.2f m)", heightCm / 100.0f);
    }
    Serial.printf(" [Raw Ultra Dist: %.2f cm, Echo: %ld us]\n",
                  lastUltrasonicDistCm, lastEchoDurationUs);

    if (WiFi.status() == WL_CONNECTED) {
      sendReading(weightKg, heightCm);
    } else {
      Serial.println("[WARN] Wi-Fi disconnected — reading not sent");
    }
  }
}

// ═════════════════════════════════════════════════════════
// WI-FI CONNECTION
// ═════════════════════════════════════════════════════════
const char* wifiStatusText(wl_status_t status) {
  switch (status) {
    case WL_IDLE_STATUS:     return "IDLE";
    case WL_NO_SSID_AVAIL:   return "SSID_NOT_FOUND (check 2.4GHz / hotspot)";
    case WL_SCAN_COMPLETED:  return "SCAN_COMPLETED";
    case WL_CONNECTED:       return "CONNECTED";
    case WL_CONNECT_FAILED:  return "CONNECT_FAILED (check password)";
    case WL_CONNECTION_LOST: return "CONNECTION_LOST";
    case WL_DISCONNECTED:    return "DISCONNECTED";
    default:                 return "UNKNOWN";
  }
}

void setupWiFi() {
  WiFi.persistent(false);
  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.printf("[WIFI] Connecting to '%s'", WIFI_SSID);
  unsigned long startAttempt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < WIFI_CONNECT_TIMEOUT_MS) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  lastWifiAttempt = millis();
  wl_status_t status = WiFi.status();
  if (status == WL_CONNECTED) {
    wasWifiConnected = true;
    Serial.printf("[WIFI] Connected! IP: %s | Signal: %d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else {
    wasWifiConnected = false;
    Serial.printf("[WIFI] Initial connection failed (%s) — retrying every %ds\n",
                  wifiStatusText(status), WIFI_RECONNECT_INTERVAL_MS / 1000);
  }
}

// ═════════════════════════════════════════════════════════
// HX711 SCALE (Weight)
// ═════════════════════════════════════════════════════════
bool waitForScaleReady(unsigned long timeoutMs) {
  unsigned long start = millis();
  while (!scale.is_ready()) {
    if (millis() - start >= timeoutMs) {
      return false;
    }
    delay(10);
  }
  return true;
}

void setupScale() {
  Serial.println("[SCALE] Initializing HX711...");
  Serial.printf("[SCALE] Pins — DOUT: GPIO%d, SCK: GPIO%d\n", HX711_DOUT_PIN, HX711_SCK_PIN);

  scale.begin(HX711_DOUT_PIN, HX711_SCK_PIN);

  // Wait for HX711 to be ready
  if (waitForScaleReady(2000)) {
    scaleReady = true;
    Serial.printf("[SCALE] HX711 ready. Calibration factor: %.1f\n", CALIBRATION_FACTOR);

    scale.set_scale(CALIBRATION_FACTOR);

    // Let bridge/ADC settle and discard initial startup samples before taring
    Serial.println("[SCALE] Settling & taring... ensure platform is empty.");
    delay(500);
    scale.read_average(10);
    scale.tare(30);  // Average 30 readings for stable zero offset
    lastRawReading = scale.get_offset();
    hasPrevRaw = true;
    idleStableCount = 0;
    Serial.printf("[SCALE] Tare complete. Zero offset: %ld\n", scale.get_offset());
  } else {
    scaleReady = false;
    Serial.println("[SCALE] ERROR: HX711 not responding! Check wiring.");
  }
}

long readRawWeight() {
  if (!scaleReady || !waitForScaleReady(200)) {
    return 0;
  }
  return scale.read_average(WEIGHT_SAMPLES);
}

float readWeight() {
  if (!scaleReady || !waitForScaleReady(200)) {
    idleStableCount = 0;
    return -1.0f;  // Sensor error
  }

  // Read raw 24-bit ADC counts once so Raw and Weight come from the exact same sample
  long raw = scale.read_average(WEIGHT_SAMPLES);
  long deltaRaw = hasPrevRaw ? ((raw >= lastRawReading) ? (raw - lastRawReading) : (lastRawReading - raw)) : 0;
  lastRawReading = raw;
  hasPrevRaw = true;

  // Calculate calibrated weight: (raw - offset) / calibration_factor
  float weight = (float)(raw - scale.get_offset()) / CALIBRATION_FACTOR;
  float deltaKg = (float)deltaRaw / CALIBRATION_FACTOR;

  // Auto-zero slow negative drift when unloaded so idle stays at 0.000 kg
  if (weight < -WEIGHT_THRESHOLD && deltaKg < IDLE_DRIFT_MAX_STEP_KG) {
    idleStableCount++;
    if (idleStableCount >= IDLE_REZERO_SAMPLES) {
      scale.set_offset(raw);
      weight = 0.0f;
      idleStableCount = 0;
      Serial.printf("[SCALE] Auto-zeroed negative drift -> new offset: %ld\n", raw);
    }
  } else {
    idleStableCount = 0;
  }

  // Clamp negative values and zero-band noise to 0.000 kg
  if (weight < WEIGHT_THRESHOLD) {
    weight = 0.0f;
  }

  return weight;
}

// ═════════════════════════════════════════════════════════
// HC-SR04 ULTRASONIC (Height)
// ═════════════════════════════════════════════════════════
void setupUltrasonic() {
  Serial.println("[ULTRA] Initializing HC-SR04...");
  Serial.printf("[ULTRA] Pins — TRIG: GPIO%d, ECHO: GPIO%d\n", TRIG_PIN, ECHO_PIN);
  Serial.printf("[ULTRA] Sensor mount height: %.2f cm\n", SENSOR_MOUNT_HEIGHT_CM);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW);

  if (ENABLE_TEST_RANGE) {
    Serial.printf("[ULTRA] Test range enabled: max height = %.2f cm\n", TEST_MAX_HEIGHT_CM);
  }

  Serial.println("[ULTRA] HC-SR04 ready.");
}

float readDistanceCm() {
  // Send 10µs trigger pulse
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  // Measure echo pulse duration (timeout after ~25ms ≈ 4m range)
  long duration = pulseIn(ECHO_PIN, HIGH, 25000);
  lastEchoDurationUs = duration;

  if (duration == 0) {
    lastUltrasonicDistCm = -1.0f;
    return -1.0f;
  }

  // Calculate distance: time × speed_of_sound / 2 (round trip)
  float distance = (duration * SOUND_SPEED_CM_PER_US) / 2.0f;
  lastUltrasonicDistCm = distance;

  // Reject obviously invalid readings
  if (distance < 2.0f || distance > MAX_DISTANCE_CM) {
    return -1.0f;
  }

  return distance;
}

float readHeightCm() {
  float distance = readDistanceCm();

  if (distance < 0) {
    return -1.0f;  // Sensor error
  }

  // Height = sensor mount height - distance to object
  float height = SENSOR_MOUNT_HEIGHT_CM - distance;

  // Reject negative heights (object above sensor — impossible)
  if (height < 0.0f) {
    return -1.0f;
  }

  // Apply test range restriction if enabled
  if (ENABLE_TEST_RANGE && height > TEST_MAX_HEIGHT_CM) {
    Serial.printf("[ULTRA] Height %.2f cm exceeds test range (%.2f cm) — flagged\n",
                  height, TEST_MAX_HEIGHT_CM);
    return -2.0f;  // -2 signals "out of test range"
  }

  return height;
}

// ═════════════════════════════════════════════════════════
// BACKEND COMMUNICATION
// ═════════════════════════════════════════════════════════
void sendReading(float weightKg, float heightCm) {
  HTTPClient http;

  String url = String(BACKEND_URL) + "/api/readings";

  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", API_KEY);
  http.setTimeout(HTTP_TIMEOUT_MS);

  long currentOffset = scaleReady ? scale.get_offset() : 0;
  String payload = "{\"deviceId\":\"" + String(DEVICE_ID) + "\","
                   "\"weightKg\":\"" + String(weightKg, 3) + "\","
                   "\"rawWeight\":" + String(lastRawReading) + ","
                   "\"rawOffset\":" + String(currentOffset) + ","
                   "\"heightCm\":\"" + String(heightCm, 2) + "\","
                   "\"timestamp\":\"" + getTimestamp() + "\","
                   "\"status\":\"" + getStatus(weightKg, heightCm) + "\"}";

  Serial.printf("[HTTP] POST %s\n", url.c_str());

  int httpCode = http.POST(payload);

  if (httpCode > 0) {
    String response = http.getString();
    if (httpCode == 200 || httpCode == 201) {
      consecutiveHttpFailures = 0;
      Serial.printf("[HTTP] Success (%d): %s\n", httpCode, response.c_str());
    } else {
      consecutiveHttpFailures++;
      Serial.printf("[HTTP] Server error (%d): %s\n", httpCode, response.c_str());
    }
  } else {
    consecutiveHttpFailures++;
    Serial.printf("[HTTP] Request failed: %s (failure #%d)\n",
                  http.errorToString(httpCode).c_str(), consecutiveHttpFailures);
  }

  http.end();

  if (consecutiveHttpFailures > 0 && consecutiveHttpFailures % 10 == 0) {
    Serial.printf("[WARN] %d consecutive HTTP failures. Check backend URL and network.\n",
                  consecutiveHttpFailures);
  }
}

// ─── Helpers ────────────────────────────────────────────
String getTimestamp() {
  unsigned long uptime = millis() / 1000;
  unsigned long hours = uptime / 3600;
  unsigned long minutes = (uptime % 3600) / 60;
  unsigned long seconds = uptime % 60;

  char buf[16];
  snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu", hours, minutes, seconds);
  return String(buf);
}

String getStatus(float weightKg, float heightCm) {
  if (weightKg < 0 && heightCm < 0) return "error_both_sensors";
  if (weightKg < 0) return "error_weight_sensor";
  if (heightCm == -2.0f) return "height_out_of_range";
  if (heightCm < 0) return "error_height_sensor";
  if (weightKg == 0.0f && heightCm == 0.0f) return "idle";
  return "ok";
}

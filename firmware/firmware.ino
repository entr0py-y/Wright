/*
 * Wright — Smart Weighing Machine ESP32 Firmware (Bluetooth LE)
 * =============================================================
 *
 * Reads weight from HX711 (GPIO 18/19) and height from HC-SR04 (GPIO 25/26),
 * and streams JSON readings directly to the web dashboard over BLE (Web Bluetooth).
 *
 * Hardware (DO NOT CHANGE):
 *   - HX711:   DOUT=GPIO19, SCK=GPIO18
 *   - HC-SR04: TRIG=GPIO25, ECHO=GPIO26 (with voltage divider)
 *   - Four 3-wire load cells → Load Combinator → HX711
 *
 * Required Libraries:
 *   - HX711 by bogde (v0.7.5+)
 *   - BLEDevice, BLEServer, BLEUtils, BLE2902 (built-in for ESP32)
 *
 * Board: ESP32 Dev Module (ESP32-WROOM)
 */

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include "HX711.h"

// ═════════════════════════════════════════════════════════
// CONFIGURATION SETTINGS
// ═════════════════════════════════════════════════════════
// Bluetooth Low Energy (BLE)
#define BLE_DEVICE_NAME       "Wright-Scale"
#define SERVICE_UUID          "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define TELEMETRY_CHAR_UUID   "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define COMMAND_CHAR_UUID     "d6d2b6c2-5b1e-4c4b-9e77-2d5d1a9c0001"
#define DEVICE_ID             "wright-esp32-01"

// HX711 Load Cell (Weight)
#define HX711_DOUT_PIN          19
#define HX711_SCK_PIN           18
#define CALIBRATION_FACTOR      26818.5f
#define WEIGHT_SAMPLES          5
#define WEIGHT_THRESHOLD        0.05f
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
#define SERIAL_BAUD_RATE        115200

// ─── Global Objects & State ─────────────────────────────
HX711 scale;
BLEServer* pServer = nullptr;
BLECharacteristic* pTelemetryChar = nullptr;
BLECharacteristic* pCommandChar = nullptr;

volatile bool bleConnected = false;
bool oldBleConnected = false;
volatile bool tareRequested = false;

unsigned long lastSendTime = 0;
bool scaleReady = false;
long lastRawReading = 0;
bool hasPrevRaw = false;
int idleStableCount = 0;
long lastEchoDurationUs = 0;
float lastUltrasonicDistCm = -1.0f;

// ─── Function Prototypes ────────────────────────────────
void setupBLE();
void setupScale();
void tareScale();
bool waitForScaleReady(unsigned long timeoutMs);
void setupUltrasonic();
long readRawWeight();
float readWeight();
float readDistanceCm();
float readHeightCm();
void sendBleReading(float weightKg, float heightCm);
String getTimestamp();
String getStatus(float weightKg, float heightCm);

// ─── BLE Callbacks ──────────────────────────────────────
class ScaleServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* server) override {
    bleConnected = true;
    Serial.println("[BLE] Browser client connected");
  }

  void onDisconnect(BLEServer* server) override {
    bleConnected = false;
    Serial.println("[BLE] Client disconnected — will restart advertising");
  }
};

class CommandCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* characteristic) override {
    String cmd = String(characteristic->getValue().c_str());
    cmd.trim();
    cmd.toUpperCase();
    if (cmd == "TARE" || cmd == "ZERO") {
      Serial.println("[BLE] Tare command received");
      tareRequested = true;
    }
  }
};

// ═════════════════════════════════════════════════════════
// SETUP
// ═════════════════════════════════════════════════════════
void setup() {
  Serial.begin(SERIAL_BAUD_RATE);
  delay(1000);

  Serial.println();
  Serial.println("╔══════════════════════════════════════════╗");
  Serial.println("║   Wright — Smart Weighing Machine        ║");
  Serial.println("║   ESP32 Firmware (Bluetooth LE)          ║");
  Serial.println("╚══════════════════════════════════════════╝");
  Serial.println();

  setupBLE();
  setupScale();
  setupUltrasonic();

  Serial.println();
  Serial.println("[READY] System initialized. Send 't' in Serial Monitor to tare anytime.");
  Serial.println();
}

// ═════════════════════════════════════════════════════════
// MAIN LOOP
// ═════════════════════════════════════════════════════════
void loop() {
  unsigned long now = millis();

  // Handle BLE reconnect advertising cleanly
  if (!bleConnected && oldBleConnected) {
    delay(300);  // Give BLE stack time to settle
    pServer->startAdvertising();
    Serial.println("[BLE] Advertising restarted — ready for connection");
    oldBleConnected = false;
  }
  if (bleConnected && !oldBleConnected) {
    oldBleConnected = true;
  }

  // Handle manual tare via Serial ('t') or BLE ("TARE")
  while (Serial.available() > 0) {
    char c = (char)Serial.read();
    if (c == 't' || c == 'T' || c == 'z' || c == 'Z') {
      tareRequested = true;
    }
  }
  if (tareRequested) {
    tareRequested = false;
    tareScale();
  }

  // Read sensors and notify BLE client at SEND_INTERVAL_MS
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

    if (bleConnected) {
      sendBleReading(weightKg, heightCm);
    } else {
      Serial.println("[BLE] Waiting for browser connection...");
    }
  }
}

// ═════════════════════════════════════════════════════════
// BLUETOOTH LOW ENERGY (GATT Server)
// ═════════════════════════════════════════════════════════
void setupBLE() {
  Serial.printf("[BLE] Initializing '%s'...\n", BLE_DEVICE_NAME);

  BLEDevice::init(BLE_DEVICE_NAME);
  BLEDevice::setMTU(256);

  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new ScaleServerCallbacks());

  BLEService* pService = pServer->createService(SERVICE_UUID);

  pTelemetryChar = pService->createCharacteristic(
    TELEMETRY_CHAR_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );
  pTelemetryChar->addDescriptor(new BLE2902());

  pCommandChar = pService->createCharacteristic(
    COMMAND_CHAR_UUID,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
  );
  pCommandChar->setCallbacks(new CommandCallbacks());

  pService->start();

  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06);
  pAdvertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();

  Serial.printf("[BLE] Advertising as '%s' (Service: %s)\n", BLE_DEVICE_NAME, SERVICE_UUID);
}

void sendBleReading(float weightKg, float heightCm) {
  long currentOffset = scaleReady ? scale.get_offset() : 0;

  // Compact newline-delimited JSON so it fits well inside negotiated BLE MTU
  String payload = "{\"d\":\"" + String(DEVICE_ID) + "\","
                   "\"w\":" + String(weightKg, 3) + ","
                   "\"r\":" + String(lastRawReading) + ","
                   "\"o\":" + String(currentOffset) + ","
                   "\"h\":" + String(heightCm, 2) + ","
                   "\"u\":" + String(lastUltrasonicDistCm, 2) + ","
                   "\"t\":\"" + getTimestamp() + "\","
                   "\"s\":\"" + getStatus(weightKg, heightCm) + "\"}\n";

  pTelemetryChar->setValue((uint8_t*)payload.c_str(), payload.length());
  pTelemetryChar->notify();
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

void tareScale() {
  if (!scaleReady) {
    Serial.println("[SCALE] Cannot tare — HX711 not ready");
    return;
  }
  Serial.println("[SCALE] Taring... ensure platform is empty.");
  delay(200);
  scale.read_average(5);
  scale.tare(30);
  lastRawReading = scale.get_offset();
  hasPrevRaw = true;
  idleStableCount = 0;
  Serial.printf("[SCALE] Tare complete. Zero offset: %ld\n", scale.get_offset());
}

void setupScale() {
  Serial.println("[SCALE] Initializing HX711...");
  Serial.printf("[SCALE] Pins — DOUT: GPIO%d, SCK: GPIO%d\n", HX711_DOUT_PIN, HX711_SCK_PIN);

  scale.begin(HX711_DOUT_PIN, HX711_SCK_PIN);

  if (waitForScaleReady(2000)) {
    scaleReady = true;
    Serial.printf("[SCALE] HX711 ready. Calibration factor: %.1f\n", CALIBRATION_FACTOR);
    scale.set_scale(CALIBRATION_FACTOR);
    delay(400);
    scale.read_average(10);
    tareScale();
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
    return -1.0f;
  }

  long raw = scale.read_average(WEIGHT_SAMPLES);
  long deltaRaw = hasPrevRaw ? ((raw >= lastRawReading) ? (raw - lastRawReading) : (lastRawReading - raw)) : 0;
  lastRawReading = raw;
  hasPrevRaw = true;

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
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, 25000);
  lastEchoDurationUs = duration;

  if (duration == 0) {
    lastUltrasonicDistCm = -1.0f;
    return -1.0f;
  }

  float distance = (duration * SOUND_SPEED_CM_PER_US) / 2.0f;
  lastUltrasonicDistCm = distance;

  if (distance < 2.0f || distance > MAX_DISTANCE_CM) {
    return -1.0f;
  }

  return distance;
}

float readHeightCm() {
  float distance = readDistanceCm();

  if (distance < 0) {
    return -1.0f;
  }

  float height = SENSOR_MOUNT_HEIGHT_CM - distance;

  if (height < 0.0f) {
    return -1.0f;
  }

  if (ENABLE_TEST_RANGE && height > TEST_MAX_HEIGHT_CM) {
    Serial.printf("[ULTRA] Height %.2f cm exceeds test range (%.2f cm) — flagged\n",
                  height, TEST_MAX_HEIGHT_CM);
    return -2.0f;
  }

  return height;
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

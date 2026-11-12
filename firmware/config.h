/*
 * Wright — Smart Weighing Machine ESP32 Firmware
 * ================================================
 * Configuration file — BLE + Sensor settings.
 */

#ifndef CONFIG_H
#define CONFIG_H

// ─── Bluetooth Low Energy (BLE) ─────────────────────────
#define BLE_DEVICE_NAME       "Wright-Scale"
#define SERVICE_UUID          "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define TELEMETRY_CHAR_UUID   "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define COMMAND_CHAR_UUID     "d6d2b6c2-5b1e-4c4b-9e77-2d5d1a9c0001"

// ─── Device Identity ────────────────────────────────────
#define DEVICE_ID             "wright-esp32-01"

// ─── HX711 Load Cell Configuration ──────────────────────
// GPIO pins — DO NOT CHANGE (matches existing wiring)
#define HX711_DOUT_PIN  19
#define HX711_SCK_PIN   18

// Provisional calibration factor — adjust after calibration
#define CALIBRATION_FACTOR  26818.5f

// Number of readings to average for stable weight
#define WEIGHT_SAMPLES  5

// Minimum weight threshold in kg (below this = 0)
#define WEIGHT_THRESHOLD        0.05f

// Auto-zero negative drift when consecutive unloaded samples change by less than this (kg)
#define IDLE_DRIFT_MAX_STEP_KG  0.30f
#define IDLE_REZERO_SAMPLES     3

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

// ─── Serial Debug ───────────────────────────────────────
#define SERIAL_BAUD_RATE  115200

#endif // CONFIG_H

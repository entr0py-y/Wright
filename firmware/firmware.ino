/*
 * Wright — Smart Weighing Machine ESP32 Firmware
 * ================================================
 * 
 * Reads weight from HX711 (GPIO 18/19) and height from HC-SR04 (GPIO 25/26),
 * then sends JSON readings to the backend API over HTTP(S).
 * 
 * Wi-Fi credentials are stored in non-volatile flash (Preferences/NVS).
 * If connection fails, the ESP32 automatically opens a password-protected
 * setup hotspot (AP) with a captive configuration portal.
 * 
 * Hardware (DO NOT CHANGE):
 *   - HX711:   DOUT=GPIO19, SCK=GPIO18
 *   - HC-SR04: TRIG=GPIO25, ECHO=GPIO26 (with voltage divider)
 *   - Four 3-wire load cells → Load Combinator → HX711
 * 
 * Required Libraries:
 *   - HX711 by bogde (v0.7.5+)
 *   (All other libraries WiFi, WebServer, Preferences, HTTPClient are built into ESP32)
 *   - WiFi          (built-in for ESP32)
 *   - HTTPClient    (built-in for ESP32)
 *   - WebServer     (built-in for ESP32)
 *   - DNSServer     (built-in for ESP32)
 *   - Preferences   (built-in for ESP32)
 * 
 * Board: ESP32 Dev Module (ESP32-WROOM)
 *
 * ─── State Machine ────────────────────────────────────────
 *
 *   ┌──────────────┐    saved creds exist     ┌──────────────┐
 *   │  BOOT        │ ───────────────────────►  │ CONNECTING   │
 *   │              │    no saved creds         │              │
 *   │              │ ────────────────┐         │  (timeout)   │
 *   └──────────────┘                 │         └──────┬───────┘
 *                                    │                │ success
 *                                    ▼                ▼
 *                            ┌──────────────┐  ┌──────────────┐
 *                            │ PORTAL_ACTIVE│  │  CONNECTED   │◄────┐
 *                            │  (AP + Web)  │  │  (Normal Op) │     │
 *                            │              │  └──────┬───────┘     │
 *                            │  new creds   │         │ lost        │
 *                            │  submitted   │         ▼             │
 *                            │      │       │  ┌──────────────┐     │
 *                            │      ▼       │  │ RECONNECTING │     │
 *                            │  VALIDATING  │  │              │─────┘
 *                            │ (AP_STA mode)│  │  (timeout)   │ reconnected
 *                            │      │       │  └──────┬───────┘
 *                            │  ok: save    │         │ exceeded
 *                            │  & connect   │         │ threshold
 *                            │      │       │         │
 *                            │  fail: keep  │◄────────┘
 *                            │  portal open │
 *                            └──────────────┘
 */

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include "HX711.h"

// ═════════════════════════════════════════════════════════
// CONFIGURATION SETTINGS
// ═════════════════════════════════════════════════════════
#define WIFI_SSID_DEFAULT       "Heisenberg69"
#define WIFI_PASSWORD_DEFAULT   "11111111"

#ifndef WIFI_SSID
#define WIFI_SSID               WIFI_SSID_DEFAULT
#endif
#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD           WIFI_PASSWORD_DEFAULT
#endif

// Wi-Fi Setup Hotspot (AP Mode)
#define AP_SSID             "SmartScale-Setup"
#define AP_PASSWORD         "WrightScale2026!"
#define AP_IP               IPAddress(192, 168, 4, 1)

// Timeouts (ms)
#define WIFI_CONNECT_TIMEOUT_MS     30000
#define WIFI_RECONNECT_INTERVAL_MS  5000
#define WIFI_PORTAL_TRIGGER_MS      60000
#define PORTAL_IDLE_TIMEOUT_MS      0
#define PORTAL_CONNECT_ATTEMPT_MS   15000

// Backend API
#define BACKEND_URL     "https://wright-y01b.onrender.com"
#define API_KEY         "change-me-to-a-long-random-string"
#define DEVICE_ID       "wright-esp32-01"

// HX711 Load Cell (Weight)
#define HX711_DOUT_PIN  19
#define HX711_SCK_PIN   18
#define CALIBRATION_FACTOR  16233.3f
#define WEIGHT_SAMPLES  5
#define WEIGHT_THRESHOLD  0.05f

// HC-SR04 Ultrasonic (Height)
#define TRIG_PIN        25
#define ECHO_PIN        26
#define SENSOR_MOUNT_HEIGHT_CM  243.84f  // 8 feet (243.84 cm)
#define MAX_DISTANCE_CM         400.0f
#define SOUND_SPEED_CM_PER_US   0.0343f
#define ENABLE_TEST_RANGE       true
#define TEST_MAX_HEIGHT_CM      243.84f  // 8 feet (243.84 cm)

// Timing & Serial
#define SEND_INTERVAL_MS        1000
#define HTTP_TIMEOUT_MS         5000
#define SERIAL_BAUD_RATE        115200

// NVS Storage Keys
#define NVS_NAMESPACE   "wright"
#define NVS_KEY_SSID    "wifi_ssid"
#define NVS_KEY_PASS    "wifi_pass"
#define NVS_KEY_SAVED   "wifi_saved"

// ─── System State ───────────────────────────────────────
enum SystemState {
  STATE_CONNECTING,         // Trying to connect to saved Wi-Fi at boot
  STATE_CONNECTED,          // Normal operation — sending data
  STATE_RECONNECTING,       // Lost Wi-Fi, retrying connection
  STATE_PORTAL_ACTIVE,      // AP + config portal running
  STATE_PORTAL_VALIDATING   // Testing new credentials while AP stays up
};

SystemState currentState = STATE_CONNECTING;

// ─── Global Objects ─────────────────────────────────────
HX711 scale;
Preferences prefs;
WebServer portalServer(80);
DNSServer dnsServer;

// ─── Wi-Fi Credential Storage ───────────────────────────
String savedSSID = "";
String savedPassword = "";
String previousSSID = "";         // Backup for rollback on validation failure
String previousPassword = "";
String pendingSSID = "";          // Credential candidate being validated
String pendingPassword = "";

// ─── State Variables ────────────────────────────────────
unsigned long lastSendTime = 0;
unsigned long lastWifiAttempt = 0;
unsigned long disconnectedSince = 0;      // When Wi-Fi was lost
unsigned long connectingStartedAt = 0;    // When current connect attempt began
unsigned long validationStartTime = 0;    // When credential validation began
unsigned long transitionTimer = 0;        // Timer before closing AP on validation success
bool scaleReady = false;
int consecutiveHttpFailures = 0;
bool portalActive = false;

// Portal state for the web UI
String portalMessage = "";
String portalMessageType = "";    // "success", "error", "info"
String validationState = "idle";   // "idle", "testing", "success", "failed"
String validationErrorMsg = "";

// ─── Function Prototypes ────────────────────────────────
// Wi-Fi & Portal
void loadCredentials();
void saveCredentials(const String& ssid, const String& password);
void clearCredentials();
void startConnecting();
void startPortal();
void stopPortal();
void handlePortalClient();
void onPortalRoot();
void onPortalScan();
void onPortalSave();
void onPortalStatus();
void onPortalStatusPage();
void onPortalReset();
void onPortalNotFound();
String buildPortalHTML(const String& scanResultsHtml = "");
String buildValidatingHTML(const String& targetSsid);
String scanNetworksHtml();

// Sensors
void setupScale();
void setupUltrasonic();
float readWeight();
float readHeightCm();
float readDistanceCm();

// Backend
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
  Serial.println("║   ESP32 Firmware v2.0.0 (Wi-Fi Portal)   ║");
  Serial.println("╚══════════════════════════════════════════╝");
  Serial.println();

  // Initialize sensors first (independent of Wi-Fi)
  setupScale();
  setupUltrasonic();

  // Load saved Wi-Fi credentials from NVS
  loadCredentials();

  if (savedSSID.length() > 0) {
    Serial.printf("[WIFI] Saved network: %s\n", savedSSID.c_str());
    Serial.println("[WIFI] Attempting connection...");
    currentState = STATE_CONNECTING;
    startConnecting();
  } else {
    Serial.println("[WIFI] No saved credentials — starting configuration portal");
    currentState = STATE_PORTAL_ACTIVE;
    startPortal();
  }

  Serial.println();
  Serial.println("[READY] System initialized. Starting measurement loop...");
  Serial.println();
}

// ═════════════════════════════════════════════════════════
// MAIN LOOP
// ═════════════════════════════════════════════════════════
void loop() {
  unsigned long now = millis();

  // ── State machine ──
  switch (currentState) {

    case STATE_CONNECTING: {
      if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[WIFI] Connected! IP: %s\n", WiFi.localIP().toString().c_str());
        Serial.printf("[WIFI] Signal: %d dBm\n", WiFi.RSSI());
        currentState = STATE_CONNECTED;
        disconnectedSince = 0;
        consecutiveHttpFailures = 0;
        break;
      }
      if (now - connectingStartedAt >= WIFI_CONNECT_TIMEOUT_MS) {
        Serial.println("[WIFI] Connection timeout — starting configuration portal");
        WiFi.disconnect(true);
        currentState = STATE_PORTAL_ACTIVE;
        startPortal();
      }
      break;
    }

    case STATE_CONNECTED: {
      if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WIFI] Connection lost — entering reconnection mode");
        currentState = STATE_RECONNECTING;
        disconnectedSince = now;
        lastWifiAttempt = 0;  // Force immediate retry
      }
      break;
    }

    case STATE_RECONNECTING: {
      if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[WIFI] Reconnected! IP: %s\n", WiFi.localIP().toString().c_str());
        currentState = STATE_CONNECTED;
        disconnectedSince = 0;
        consecutiveHttpFailures = 0;
        break;
      }
      // Periodically retry
      if (now - lastWifiAttempt >= WIFI_RECONNECT_INTERVAL_MS) {
        lastWifiAttempt = now;
        Serial.println("[WIFI] Reconnecting...");
        WiFi.disconnect();
        WiFi.begin(savedSSID.c_str(), savedPassword.c_str());
      }
      // If exceeded portal trigger threshold, open portal
      if (now - disconnectedSince >= WIFI_PORTAL_TRIGGER_MS) {
        Serial.printf("[WIFI] Disconnected for %lus — starting portal\n",
                      (now - disconnectedSince) / 1000);
        WiFi.disconnect(true);
        currentState = STATE_PORTAL_ACTIVE;
        startPortal();
      }
      break;
    }

    case STATE_PORTAL_ACTIVE: {
      handlePortalClient();
      break;
    }

    case STATE_PORTAL_VALIDATING: {
      handlePortalClient();

      if (WiFi.status() == WL_CONNECTED) {
        if (validationState != "success") {
          validationState = "success";
          transitionTimer = now;
          saveCredentials(pendingSSID, pendingPassword);
          Serial.printf("[PORTAL] Connection verified! Connected to %s (IP: %s)\n",
                        pendingSSID.c_str(), WiFi.localIP().toString().c_str());
        } else if (now - transitionTimer >= 3500) {
          // Allowed user browser to receive success response and display it
          Serial.println("[PORTAL] Transitioning to connected mode");
          stopPortal();
          WiFi.mode(WIFI_STA);
          currentState = STATE_CONNECTED;
          disconnectedSince = 0;
          consecutiveHttpFailures = 0;
          validationState = "idle";
        }
      } else if (now - validationStartTime >= PORTAL_CONNECT_ATTEMPT_MS) {
        // Validation timed out
        Serial.printf("[PORTAL] Failed to connect to '%s' within %d ms\n",
                      pendingSSID.c_str(), PORTAL_CONNECT_ATTEMPT_MS);
        validationState = "failed";
        validationErrorMsg = "Connection timed out. Check password or router range.";
        portalMessage = "Failed to connect to <strong>" + pendingSSID + "</strong>. Please verify the password and try again.";
        portalMessageType = "error";

        // Stop STA attempt, keep AP alive
        WiFi.disconnect(false);
        if (previousSSID.length() > 0) {
          savedSSID = previousSSID;
          savedPassword = previousPassword;
        }
        currentState = STATE_PORTAL_ACTIVE;
      }
      break;
    }
  }

  // ── Sensor measurements (ALWAYS run continuously, non-blocking) ──
  if (now - lastSendTime >= SEND_INTERVAL_MS) {
    lastSendTime = now;

    float weightKg = readWeight();
    float heightCm = readHeightCm();

    // Serial debug
    Serial.printf("[SENSOR] Weight: %.3f kg (Raw: %ld, Offset: %ld) | Height: %.2f cm",
                  weightKg, lastRawReading, (scaleReady ? scale.get_offset() : 0), heightCm);
    if (heightCm > 0) {
      Serial.printf(" (%.2f m)", heightCm / 100.0f);
    }
    Serial.println();

    // Send to backend only in connected state with active Wi-Fi
    if (currentState == STATE_CONNECTED && WiFi.status() == WL_CONNECTED) {
      sendReading(weightKg, heightCm);
    } else if (currentState != STATE_PORTAL_ACTIVE && currentState != STATE_PORTAL_VALIDATING) {
      Serial.println("[WARN] Wi-Fi disconnected — reading not sent");
    }
  }
}

// ═════════════════════════════════════════════════════════
// CREDENTIAL STORAGE (Preferences / NVS)
// ═════════════════════════════════════════════════════════
void loadCredentials() {
  prefs.begin(NVS_NAMESPACE, true);  // read-only
  bool hasSaved = prefs.getBool(NVS_KEY_SAVED, false);

  if (hasSaved) {
    savedSSID = prefs.getString(NVS_KEY_SSID, "");
    savedPassword = prefs.getString(NVS_KEY_PASS, "");
    Serial.println("[NVS] Loaded saved Wi-Fi credentials");
  } else {
    // Use compile-time defaults if no saved credentials
    savedSSID = String(WIFI_SSID_DEFAULT);
    savedPassword = String(WIFI_PASSWORD_DEFAULT);
    // Only use defaults if they're not the placeholder
    if (savedSSID == "YOUR_WIFI_SSID") {
      savedSSID = "";
      savedPassword = "";
      Serial.println("[NVS] No saved credentials and defaults are placeholders");
    } else {
      Serial.println("[NVS] Using compile-time default credentials");
    }
  }
  prefs.end();
  // Never log the password
  Serial.printf("[NVS] SSID: %s | Password: %s\n",
                savedSSID.c_str(),
                savedSSID.length() > 0 ? "****" : "(empty)");
}

void saveCredentials(const String& ssid, const String& password) {
  prefs.begin(NVS_NAMESPACE, false);  // read-write
  prefs.putString(NVS_KEY_SSID, ssid);
  prefs.putString(NVS_KEY_PASS, password);
  prefs.putBool(NVS_KEY_SAVED, true);
  prefs.end();

  savedSSID = ssid;
  savedPassword = password;
  Serial.printf("[NVS] Credentials saved for: %s\n", ssid.c_str());
}

void clearCredentials() {
  prefs.begin(NVS_NAMESPACE, false);
  prefs.clear();
  prefs.end();
  savedSSID = "";
  savedPassword = "";
  Serial.println("[NVS] All saved credentials cleared");
}

// ═════════════════════════════════════════════════════════
// WI-FI CONNECTION
// ═════════════════════════════════════════════════════════
void startConnecting() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(savedSSID.c_str(), savedPassword.c_str());
  connectingStartedAt = millis();
}

// ═════════════════════════════════════════════════════════
// CONFIGURATION PORTAL (AP + Web Server + Captive DNS)
// ═════════════════════════════════════════════════════════
void startPortal() {
  if (portalActive) return;

  Serial.println("[PORTAL] Starting configuration portal...");

  // Configure SoftAP network
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  delay(100);  // Allow AP to stabilize

  Serial.printf("[PORTAL] AP SSID: %s\n", AP_SSID);
  Serial.printf("[PORTAL] AP IP:   %s\n", WiFi.softAPIP().toString().c_str());
  Serial.println("[PORTAL] AP Password: (set in config.h)");

  // Start DNS server for captive portal redirect (port 53)
  dnsServer.start(53, "*", AP_IP);

  // Configure web server routes
  portalServer.on("/", HTTP_GET, onPortalRoot);
  portalServer.on("/scan", HTTP_GET, onPortalScan);
  portalServer.on("/save", HTTP_POST, onPortalSave);
  portalServer.on("/status", HTTP_GET, onPortalStatus);
  portalServer.on("/status-page", HTTP_GET, onPortalStatusPage);
  portalServer.on("/reset", HTTP_POST, onPortalReset);

  // Captive portal detection endpoints (redirect to root)
  portalServer.on("/generate_204", HTTP_GET, onPortalRoot);        // Android / Chrome
  portalServer.on("/gen_204", HTTP_GET, onPortalRoot);            // Android
  portalServer.on("/fwlink", HTTP_GET, onPortalRoot);              // Windows
  portalServer.on("/hotspot-detect.html", HTTP_GET, onPortalRoot); // Apple iOS/macOS
  portalServer.on("/canonical.html", HTTP_GET, onPortalRoot);      // Firefox
  portalServer.onNotFound(onPortalNotFound);

  portalServer.begin();
  portalActive = true;
  validationState = "idle";
  validationErrorMsg = "";

  portalMessage = "Connect to <strong>" + String(AP_SSID) + "</strong> Wi-Fi, "
                  "then open <strong>http://192.168.4.1</strong> in your browser.";
  portalMessageType = "info";

  Serial.println("[PORTAL] Web server started on port 80");
  Serial.println("[PORTAL] DNS captive redirect active");
}

void stopPortal() {
  if (!portalActive) return;

  portalServer.stop();
  dnsServer.stop();
  WiFi.softAPdisconnect(true);
  portalActive = false;

  Serial.println("[PORTAL] Portal stopped");
}

void handlePortalClient() {
  dnsServer.processNextRequest();
  portalServer.handleClient();
}

// ─── Portal Route: Root Page ────────────────────────────
void onPortalRoot() {
  portalServer.send(200, "text/html", buildPortalHTML());
}

// ─── Portal Route: Scan Networks ────────────────────────
void onPortalScan() {
  String html = scanNetworksHtml();
  portalServer.send(200, "text/html", buildPortalHTML(html));
}

// ─── Portal Route: Save Credentials ─────────────────────
void onPortalSave() {
  String newSSID = portalServer.arg("ssid");
  String newPassword = portalServer.arg("password");

  // Trim whitespace
  newSSID.trim();
  newPassword.trim();

  // Validate inputs
  if (newSSID.length() == 0) {
    portalMessage = "SSID cannot be empty.";
    portalMessageType = "error";
    portalServer.send(200, "text/html", buildPortalHTML());
    return;
  }
  if (newSSID.length() > 32) {
    portalMessage = "SSID too long (max 32 characters).";
    portalMessageType = "error";
    portalServer.send(200, "text/html", buildPortalHTML());
    return;
  }
  if (newPassword.length() > 0 && newPassword.length() < 8) {
    portalMessage = "Password must be at least 8 characters (or leave empty for open networks).";
    portalMessageType = "error";
    portalServer.send(200, "text/html", buildPortalHTML());
    return;
  }
  if (newPassword.length() > 63) {
    portalMessage = "Password too long (max 63 characters).";
    portalMessageType = "error";
    portalServer.send(200, "text/html", buildPortalHTML());
    return;
  }

  // Backup current credentials for rollback
  previousSSID = savedSSID;
  previousPassword = savedPassword;

  pendingSSID = newSSID;
  pendingPassword = newPassword;
  validationState = "testing";
  validationErrorMsg = "";
  validationStartTime = millis();
  transitionTimer = 0;

  Serial.printf("[PORTAL] Initiating connection test to: %s\n", pendingSSID.c_str());

  // Stay in AP_STA mode so the hotspot stays alive while testing router connection!
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(pendingSSID.c_str(), pendingPassword.c_str());

  currentState = STATE_PORTAL_VALIDATING;

  // Immediately respond to user's browser with the real-time validation screen
  portalServer.send(200, "text/html", buildValidatingHTML(pendingSSID));
}

// ─── Portal Route: Status JSON ──────────────────────────
void onPortalStatus() {
  String stateStr = (currentState == STATE_CONNECTED) ? "connected" :
                    (currentState == STATE_PORTAL_VALIDATING) ? "validating" : "portal";
  String ipStr = (WiFi.status() == WL_CONNECTED) ? WiFi.localIP().toString() : "";
  String ssidStr = (currentState == STATE_PORTAL_VALIDATING) ? pendingSSID : savedSSID;

  String json = "{\"state\":\"" + stateStr + "\","
                "\"validation\":\"" + validationState + "\","
                "\"error\":\"" + validationErrorMsg + "\","
                "\"ssid\":\"" + ssidStr + "\","
                "\"ip\":\"" + ipStr + "\","
                "\"apClients\":" + String(WiFi.softAPgetStationNum()) + ","
                "\"uptime\":" + String(millis() / 1000) + "}";

  portalServer.send(200, "application/json", json);
}

// ─── Portal Route: Status Page (Fallback) ───────────────
void onPortalStatusPage() {
  if (currentState == STATE_PORTAL_VALIDATING) {
    portalServer.send(200, "text/html", buildValidatingHTML(pendingSSID));
  } else {
    portalServer.send(200, "text/html", buildPortalHTML());
  }
}

// ─── Portal Route: Reset Credentials ────────────────────
void onPortalReset() {
  clearCredentials();
  portalMessage = "All saved Wi-Fi credentials have been cleared. "
                  "Scan for networks and enter new credentials below.";
  portalMessageType = "info";
  portalServer.send(200, "text/html", buildPortalHTML());
}

// ─── Portal Route: Captive / 404 ────────────────────────
void onPortalNotFound() {
  portalServer.sendHeader("Location", "http://192.168.4.1/", true);
  portalServer.send(302, "text/plain", "Redirecting to configuration portal...");
}

// ─── Network Scan ───────────────────────────────────────
String scanNetworksHtml() {
  Serial.println("[PORTAL] Scanning Wi-Fi networks...");
  int n = WiFi.scanNetworks();
  String html = "";

  if (n <= 0) {
    html = "<p style='color:var(--muted);text-align:center;padding:1rem;'>"
           "No networks found. Try again.</p>";
  } else {
    Serial.printf("[PORTAL] Found %d networks\n", n);

    // Sort by signal strength (strongest first)
    int indices[n];
    for (int i = 0; i < n; i++) indices[i] = i;
    for (int i = 0; i < n - 1; i++) {
      for (int j = i + 1; j < n; j++) {
        if (WiFi.RSSI(indices[j]) > WiFi.RSSI(indices[i])) {
          int tmp = indices[i];
          indices[i] = indices[j];
          indices[j] = tmp;
        }
      }
    }

    // Deduplicate SSIDs
    html = "<div class='network-list'>";
    String seen = "";
    for (int i = 0; i < n; i++) {
      String ssid = WiFi.SSID(indices[i]);
      if (ssid.length() == 0) continue;  // Skip hidden networks
      if (seen.indexOf("|" + ssid + "|") >= 0) continue;  // Skip duplicates
      seen += "|" + ssid + "|";

      int rssi = WiFi.RSSI(indices[i]);
      String security = (WiFi.encryptionType(indices[i]) == WIFI_AUTH_OPEN) ? "Open" : "🔒";
      String signalIcon;
      if (rssi > -50) signalIcon = "▂▄▆█";
      else if (rssi > -65) signalIcon = "▂▄▆░";
      else if (rssi > -75) signalIcon = "▂▄░░";
      else signalIcon = "▂░░░";

      html += "<button type='button' class='network-btn' onclick=\"selectNetwork('" + ssid + "')\">";
      html += "<span class='net-name'>" + ssid + "</span>";
      html += "<span class='net-info'>" + signalIcon + " " + String(rssi) + "dBm " + security + "</span>";
      html += "</button>";
    }
    html += "</div>";
  }

  WiFi.scanDelete();
  return html;
}

// ─── Portal HTML Page ───────────────────────────────────
String buildPortalHTML(const String& scanResultsHtml) {
  String html = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Wright — Wi-Fi Configuration</title>
<style>
  :root {
    --bg: #000000; --border: #222222; --border-strong: #ffffff;
    --text: #ffffff; --text-muted: #888888;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body {
    font-family: 'JetBrains Mono', 'SF Mono', Menlo, Consolas, monospace;
    background: var(--bg); color: var(--text); line-height: 1.5;
    min-height: 100vh; padding: 1.5rem 1rem;
    -webkit-font-smoothing: antialiased;
  }
  .container { max-width: 480px; margin: 0 auto; border: 1px solid var(--border); }
  .header {
    padding: 1.5rem 1.25rem; border-bottom: 1px solid var(--border);
  }
  .header h1 {
    font-size: 1.1rem; font-weight: 800; letter-spacing: 0.15em;
  }
  .header p { color: var(--text-muted); font-size: 0.75rem; letter-spacing: 0.05em; margin-top: 0.25rem; }
  .section {
    padding: 1.25rem; border-bottom: 1px solid var(--border);
  }
  .section-title {
    font-size: 0.75rem; font-weight: 700; color: var(--text-muted);
    letter-spacing: 0.12em; margin-bottom: 1rem;
  }
  .msg {
    padding: 0.75rem 1rem; font-size: 0.8rem; letter-spacing: 0.04em;
    border: 1px solid #ffffff; background: #ffffff; color: #000000;
    margin: 1rem; font-weight: 700;
  }
  label { display: block; font-size: 0.7rem; color: var(--text-muted); margin-bottom: 0.4rem; letter-spacing: 0.08em; }
  input[type="text"], input[type="password"] {
    width: 100%; padding: 0.75rem; border: 1px solid var(--border);
    background: #000000; color: #ffffff; font-size: 0.9rem; outline: none;
    font-family: inherit;
  }
  input:focus { border-color: #ffffff; }
  .field { margin-bottom: 1rem; }
  .btn {
    display: block; width: 100%; padding: 0.8rem; font-size: 0.8rem;
    font-weight: 700; cursor: pointer; text-decoration: none;
    letter-spacing: 0.08em; text-align: center; font-family: inherit;
    border-radius: 0;
  }
  .btn--primary { background: #ffffff; color: #000000; border: 1px solid #ffffff; }
  .btn--outline { background: transparent; border: 1px solid var(--border); color: #ffffff; }
  .btn--outline:hover { border-color: #ffffff; }
  .btn--danger { background: transparent; border: 1px solid #444444; color: #888888; font-size: 0.7rem; }
  .btn--danger:hover { color: #ffffff; border-color: #ffffff; }
  .btn + .btn { margin-top: 0.5rem; }
  .network-list { display: flex; flex-direction: column; gap: 0.5rem; max-height: 240px; overflow-y: auto; margin-bottom: 0.75rem; }
  .network-btn {
    display: flex; justify-content: space-between; align-items: center;
    width: 100%; padding: 0.65rem 0.8rem; border: 1px solid var(--border);
    background: #000000; color: #ffffff; cursor: pointer; font-size: 0.8rem;
    font-family: inherit; text-align: left;
  }
  .network-btn:hover { border-color: #ffffff; }
  .net-info { font-size: 0.7rem; color: var(--text-muted); }
  .footer { padding: 1.25rem; color: var(--text-muted); font-size: 0.7rem; letter-spacing: 0.05em; }
  .show-pass { display: flex; align-items: center; gap: 0.5rem; margin-top: 0.4rem; cursor: pointer; font-size: 0.7rem; color: var(--text-muted); }
  .show-pass input { width: auto; }
</style>
</head>
<body>
<div class="container">
  <div class="header">
    <h1>WRIGHT / SETUP</h1>
    <p>WI-FI CONFIGURATION TERMINAL</p>
  </div>
)rawhtml";

  // Status message
  if (portalMessage.length() > 0) {
    html += "<div class='msg'>" + portalMessage + "</div>";
  }

  // Scan results
  html += "<div class='section'>";
  html += "<div class='section-title'>[ 01 ] DETECTED NETWORKS</div>";

  if (scanResultsHtml.length() > 0) {
    html += scanResultsHtml;
  }

  html += "<a href='/scan' class='btn btn--outline'>SCAN FOR 2.4GHZ NETWORKS</a>";
  html += "</div>";

  // Credential form
  html += R"rawhtml(
  <div class="section">
    <div class="section-title">[ 02 ] NETWORK CREDENTIALS</div>
    <form action="/save" method="POST">
      <div class="field">
        <label for="ssid">SSID / NETWORK NAME</label>
        <input type="text" id="ssid" name="ssid" placeholder="ENTER SSID"
               maxlength="32" required autocomplete="off">
      </div>
      <div class="field">
        <label for="password">SECURITY KEY / PASSWORD</label>
        <input type="password" id="password" name="password" placeholder="ENTER PASSWORD"
               maxlength="63" autocomplete="off">
        <div class="show-pass">
          <input type="checkbox" id="showpw" onchange="document.getElementById('password').type=this.checked?'text':'password'">
          <label for="showpw">REVEAL PASSWORD</label>
        </div>
      </div>
      <button type="submit" class="btn btn--primary">SAVE & VERIFY CONNECTION</button>
    </form>
  </div>
)rawhtml";

  // Device info + reset
  html += "<div class='section'>";
  html += "<div class='section-title'>[ 03 ] TELEMETRY & NVRAM</div>";
  html += "<p style='font-size:0.75rem;color:var(--text-muted);line-height:1.8;margin-bottom:1rem;'>";
  html += "DEVICE_ID: <span style='color:#fff'>" + String(DEVICE_ID) + "</span><br>";
  html += "SAVED_SSID: <span style='color:#fff'>" + (savedSSID.length() > 0 ? savedSSID : "[NONE]") + "</span><br>";
  html += "PORTAL_IP: <span style='color:#fff'>192.168.4.1</span><br>";
  html += "UPTIME: <span style='color:#fff'>" + String(millis() / 1000) + "S</span>";
  html += "</p>";
  html += "<form action='/reset' method='POST' onsubmit=\"return confirm('CLEAR SAVED NVRAM CREDENTIALS?')\">";
  html += "<button type='submit' class='btn btn--danger'>PURGE SAVED CREDENTIALS</button>";
  html += "</form>";
  html += "</div>";

  // Manual address notice
  html += "<div class='footer'>";
  html += "PORTAL GATEWAY: 192.168.4.1<br>";
  html += "WRIGHT SYSTEM FIRMWARE V2.0";
  html += "</div>";

  // JavaScript for SSID selection
  html += R"rawhtml(
</div>
<script>
function selectNetwork(ssid) {
  document.getElementById('ssid').value = ssid;
  document.getElementById('password').focus();
}
</script>
</body></html>
)rawhtml";

  return html;
}

// ─── Real-Time Validating Screen HTML ───────────────────
String buildValidatingHTML(const String& targetSsid) {
  String html = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Wright — Verifying Link</title>
<style>
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body {
    font-family: 'JetBrains Mono', 'SF Mono', Menlo, Consolas, monospace;
    background: #000000; color: #ffffff; line-height: 1.5;
    min-height: 100vh; padding: 2rem 1rem; display: flex;
    align-items: center; justify-content: center;
  }
  .container { max-width: 480px; width: 100%; border: 1px solid #222222; }
  .card { padding: 2.5rem 1.5rem; text-align: left; }
  .tag { font-size: 0.7rem; color: #888888; letter-spacing: 0.12em; margin-bottom: 0.75rem; }
  h1 { font-size: 1.2rem; font-weight: 800; letter-spacing: 0.08em; margin-bottom: 1rem; }
  p { color: #888888; font-size: 0.8rem; line-height: 1.6; margin-bottom: 1.5rem; }
  .btn {
    display: block; width: 100%; padding: 0.8rem; font-size: 0.8rem;
    font-weight: 700; cursor: pointer; text-decoration: none;
    letter-spacing: 0.08em; text-align: center; font-family: inherit;
    background: #ffffff; color: #000000; border: 1px solid #ffffff;
  }
  .progress-line {
    height: 2px; width: 100%; background: #222222; overflow: hidden; margin-bottom: 1.5rem;
  }
  .progress-line-bar {
    width: 30%; height: 100%; background: #ffffff;
    animation: prog 1.5s ease-in-out infinite;
  }
  @keyframes prog {
    0% { transform: translateX(-100%); }
    100% { transform: translateX(400%); }
  }
</style>
</head>
<body>
<div class="container">
  <div class="card" id="card">
    <div class="tag" id="tag">[ STATUS: TESTING LINK ]</div>
    <div class="progress-line" id="prog"><div class="progress-line-bar"></div></div>
    <h1 id="title">VERIFYING CONNECTION</h1>
    <p id="desc">ESTABLISHING LINK TO <strong>)rawhtml";
  html += targetSsid;
  html += R"rawhtml(</strong>...<br>TEST TIMEOUT: 15 SECONDS</p>
    <div id="action" style="display:none;"></div>
  </div>
</div>
<script>
let attempts = 0;
const poll = setInterval(async () => {
  attempts++;
  try {
    const res = await fetch('/status');
    const data = await res.json();
    if (data.validation === 'success' || data.state === 'connected') {
      clearInterval(poll);
      document.getElementById('prog').style.display = 'none';
      document.getElementById('tag').innerText = '[ STATUS: VERIFIED ]';
      document.getElementById('title').innerText = 'LINK ESTABLISHED';
      document.getElementById('desc').innerHTML = 'CONNECTED TO: <strong>' + data.ssid + '</strong><br>' +
        (data.ip ? 'LOCAL IP: <strong>' + data.ip + '</strong><br>' : '') +
        '<span style="color:#555555">SAVED TO NVRAM. SETUP HOTSPOT CLOSING. DATA STREAMING TO DASHBOARD.</span>';
    } else if (data.validation === 'failed') {
      clearInterval(poll);
      document.getElementById('prog').style.display = 'none';
      document.getElementById('tag').innerText = '[ STATUS: FAILED ]';
      document.getElementById('title').innerText = 'LINK FAILURE';
      document.getElementById('desc').innerHTML = (data.error || 'INVALID CREDENTIALS OR ROUTER UNREACHABLE.') + '<br><span style="color:#555555">HOTSPOT REMAINS ACTIVE FOR RETRY.</span>';
      document.getElementById('action').style.display = 'block';
      document.getElementById('action').innerHTML = '<a href="/" class="btn">RETURN TO SETUP</a>';
    }
  } catch (err) {
    if (attempts > 12) {
      clearInterval(poll);
      document.getElementById('prog').style.display = 'none';
      document.getElementById('title').innerText = 'CONFIGURATION DISPATCHED';
      document.getElementById('desc').innerHTML = 'TEST CYCLE COMPLETED.<br>IF LINK SUCCEEDED, SCALE IS SENDING MEASUREMENTS.<br>IF FAILED, HOTSPOT WILL REMAIN ACCESSIBLE.';
      document.getElementById('action').style.display = 'block';
      document.getElementById('action').innerHTML = '<a href="/" class="btn">CHECK STATUS</a>';
    }
  }
}, 1200);
</script>
</body>
</html>
)rawhtml";

  return html;
}

// ═════════════════════════════════════════════════════════
// HX711 SCALE (Weight) — UNCHANGED
// ═════════════════════════════════════════════════════════
void setupScale() {
  Serial.println("[SCALE] Initializing HX711...");
  Serial.printf("[SCALE] Pins — DOUT: GPIO%d, SCK: GPIO%d\n", HX711_DOUT_PIN, HX711_SCK_PIN);

  scale.begin(HX711_DOUT_PIN, HX711_SCK_PIN);

  // Wait for HX711 to be ready
  int waitCount = 0;
  while (!scale.is_ready() && waitCount < 20) {
    delay(100);
    waitCount++;
  }

  if (scale.is_ready()) {
    scaleReady = true;
    Serial.printf("[SCALE] HX711 ready. Calibration factor: %.1f\n", CALIBRATION_FACTOR);

    // Set calibration factor
    scale.set_scale(CALIBRATION_FACTOR);

    // Tare at startup (zero the scale with nothing on it)
    Serial.println("[SCALE] Taring... ensure platform is empty.");
    scale.tare(20);  // Average 20 readings for tare
    Serial.printf("[SCALE] Tare complete. Zero offset: %ld\n", scale.get_offset());
  } else {
    scaleReady = false;
    Serial.println("[SCALE] ERROR: HX711 not responding! Check wiring.");
  }
}

long lastRawReading = 0;

long readRawWeight() {
  if (!scaleReady || !scale.is_ready()) {
    return 0;
  }
  return scale.read_average(WEIGHT_SAMPLES);
}

float readWeight() {
  if (!scaleReady || !scale.is_ready()) {
    return -1.0f;  // Sensor error
  }

  // Read raw 24-bit ADC counts
  lastRawReading = scale.read_average(WEIGHT_SAMPLES);

  // Get calibrated reading: (raw - offset) / calibration_factor
  float weight = scale.get_units(WEIGHT_SAMPLES);

  // Apply threshold — ignore noise near zero
  if (weight < WEIGHT_THRESHOLD && weight > -WEIGHT_THRESHOLD) {
    weight = 0.0f;
  }

  return weight;
}

// ═════════════════════════════════════════════════════════
// HC-SR04 ULTRASONIC (Height) — UNCHANGED
// ═════════════════════════════════════════════════════════
void setupUltrasonic() {
  Serial.println("[ULTRA] Initializing HC-SR04...");
  Serial.printf("[ULTRA] Pins — TRIG: GPIO%d, ECHO: GPIO%d\n", TRIG_PIN, ECHO_PIN);
  Serial.printf("[ULTRA] Sensor mount height: %.2f cm\n", SENSOR_MOUNT_HEIGHT_CM);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  // Ensure trigger starts LOW
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

  if (duration == 0) {
    // Timeout — no echo received
    return -1.0f;
  }

  // Calculate distance: time × speed_of_sound / 2 (round trip)
  float distance = (duration * SOUND_SPEED_CM_PER_US) / 2.0f;

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
// BACKEND COMMUNICATION — UNCHANGED
// ═════════════════════════════════════════════════════════
void sendReading(float weightKg, float heightCm) {
  HTTPClient http;

  String url = String(BACKEND_URL) + "/api/readings";

  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", API_KEY);
  http.setTimeout(HTTP_TIMEOUT_MS);

  // Build JSON payload without external library
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

  // Log if we're having persistent issues (never restart)
  if (consecutiveHttpFailures > 0 && consecutiveHttpFailures % 10 == 0) {
    Serial.printf("[WARN] %d consecutive HTTP failures. Check backend URL and network.\n",
                  consecutiveHttpFailures);
  }
}

// ─── Helpers — UNCHANGED ────────────────────────────────
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

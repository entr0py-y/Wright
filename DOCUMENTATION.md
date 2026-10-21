# Wright — Wi-Fi Connection & Setup Hotspot Guide

This guide explains how the ESP32 manages Wi-Fi, when it opens its setup hotspot, and how to connect it to any new Wi-Fi network without touching any code.

---

## 1. When Does the ESP32 Create Its Setup Hotspot?

The ESP32 automatically creates its setup hotspot (**`SmartScale-Setup`**) in two exact scenarios:

### Scenario A: First Boot / No Saved Credentials
- When powered on for the very first time (or after clearing memory).
- If it finds no saved Wi-Fi network in its internal storage, it launches the setup hotspot immediately.

### Scenario B: Saved Wi-Fi Is Unavailable or Changed
- When you turn on the scale in a new location, or turn off your phone hotspot, or change your router password.
- The ESP32 tries to find and connect to its previously saved Wi-Fi for **30 seconds**.
- If the router is unreachable or the password was changed, it automatically gives up and opens the setup hotspot (**`SmartScale-Setup`**).

---

## 2. Setup Hotspot Details

| Setting | Value |
| :--- | :--- |
| **Hotspot Name (SSID)** | `SmartScale-Setup` |
| **Hotspot Password** | `WrightScale2026!` |
| **Configuration Page URL** | `http://192.168.4.1` |

---

## 3. Step-by-Step: How to Connect to a New Wi-Fi Network

Follow these steps from any smartphone, tablet, or laptop:

### Step 1: Connect to the Scale's Hotspot
1. Turn on the weighing scale.
2. If your old Wi-Fi is off, wait ~30 seconds for the hotspot to appear.
3. Open **Wi-Fi Settings** on your phone or laptop.
4. Select and connect to:
   - **Wi-Fi Name**: `SmartScale-Setup`
   - **Password**: `WrightScale2026!`

---

### Step 2: Open the Setup Portal
1. Open any web browser (Chrome, Safari, Brave, etc.).
2. Go to:
   $$\mathbf{http://192.168.4.1}$$
   *(On most phones, a "Sign in to network" prompt will open this page automatically).*

---

### Step 3: Select Your Network & Save
1. Tap **[ SCAN NETWORKS ]** to view all available 2.4 GHz Wi-Fi networks nearby.
2. Tap your Wi-Fi network from the list (or type the SSID manually).
3. Enter your Wi-Fi password.
4. Tap **CONNECT SCALE** (or **SAVE & VERIFY CONNECTION**).

---

### Step 4: Automatic Verification & Handover
1. The scale will test the connection to your Wi-Fi while keeping the hotspot alive.
2. If connection is successful:
   - It saves the new credentials permanently into non-volatile flash memory (NVS).
   - It shuts down the setup hotspot (`SmartScale-Setup`).
   - It connects to your Wi-Fi and starts streaming data to the cloud dashboard immediately.
3. If password was incorrect:
   - The hotspot stays open and prompts you to re-enter the password.

---

## 4. Helpful Tips & Troubleshooting

- **Memory Retention**: Saved Wi-Fi credentials stay stored even when the scale is unplugged or powered off. You only need to do this setup once per network.
- **2.4 GHz Wi-Fi Only**: ESP32 hardware supports **2.4 GHz Wi-Fi** bands (standard on all routers and phone hotspots). If using dual-band Wi-Fi, ensure 2.4 GHz is enabled.
- **To Force Hotspot Mode Anytime**: If you ever want to force the setup hotspot to appear, simply turn off your phone's hotspot or walk away from your router and power-cycle the scale. After 30 seconds of searching, the `SmartScale-Setup` hotspot will appear.

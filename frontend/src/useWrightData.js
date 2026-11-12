/**
 * Wright — Web Bluetooth Client Hook
 * ===================================
 * Connects directly to the ESP32 BLE GATT server ("Wright-Scale")
 * via the browser's Web Bluetooth API and streams live sensor readings.
 */

import { useState, useEffect, useCallback, useRef } from 'react';

const BLE_SERVICE_UUID = '4fafc201-1fb5-459e-8fcc-c5c9c331914b';
const BLE_TELEMETRY_CHAR_UUID = 'beb5483e-36e1-4688-b7f5-ea07361b26a8';
const BLE_COMMAND_CHAR_UUID = 'd6d2b6c2-5b1e-4c4b-9e77-2d5d1a9c0001';

const MAX_HISTORY = 50;
const STALE_THRESHOLD_MS = 10000;

/**
 * Multi-point piecewise calibration curve matching backend/frontend calibration.
 */
export function computeCalibratedWeightKg(rawWeight, rawOffset, fallbackWeightKg) {
  if (rawWeight !== undefined && rawWeight !== null && rawOffset !== undefined && rawOffset !== null) {
    const net = rawWeight - rawOffset;
    let val = 0.0;
    if (net > 1200) {
      if (net <= 34181) {
        val = (net / 34181) * 2.20;
      } else if (net <= 92810) {
        val = 2.20 + ((net - 34181) / (92810 - 34181)) * (4.50 - 2.20);
      } else if (net <= 146901) {
        val = 4.50 + ((net - 92810) / (146901 - 92810)) * (6.00 - 4.50);
      } else {
        val = 6.00 + ((net - 146901) / 26860.0);
      }
    }
    if (val < 0.1 && val > -0.1) val = 0.0;
    if (val < 0) val = 0.0;
    return parseFloat(val.toFixed(3));
  }
  return fallbackWeightKg !== undefined && fallbackWeightKg !== null ? Number(fallbackWeightKg) : 0;
}

function normalizeBlePacket(parsed) {
  const status = parsed.s ?? parsed.status ?? 'ok';
  const rawWeight = parsed.r !== undefined ? Number(parsed.r) : (parsed.rawWeight ?? null);
  const rawOffset = parsed.o !== undefined ? Number(parsed.o) : (parsed.rawOffset ?? null);
  const rawFallbackWeight = parsed.w !== undefined ? Number(parsed.w) : Number(parsed.weightKg ?? 0);

  const isWeightFault = status === 'error_weight_sensor' || status === 'error_both_sensors' || rawFallbackWeight < 0;
  const weightKg = isWeightFault
    ? -1
    : computeCalibratedWeightKg(rawWeight, rawOffset, rawFallbackWeight);

  const now = Date.now();
  return {
    deviceId: parsed.d ?? parsed.deviceId ?? 'wright-esp32-01',
    weightKg,
    rawWeight,
    rawOffset,
    heightCm: parsed.h !== undefined ? Number(parsed.h) : Number(parsed.heightCm ?? -1),
    rawUltrasonicDistCm: parsed.u !== undefined ? Number(parsed.u) : null,
    status,
    deviceTimestamp: parsed.t ?? parsed.timestamp ?? '—',
    serverTimestamp: new Date(now).toISOString(),
    receivedAt: now,
    ageMs: 0,
    isOnline: true,
    isStale: false,
  };
}

export function useWrightData() {
  const bleSupported = typeof navigator !== 'undefined' && 'bluetooth' in navigator;

  const [data, setData] = useState(null);
  const [history, setHistory] = useState([]);
  const [error, setError] = useState(null);
  const [isConnecting, setIsConnecting] = useState(false);
  const [isConnected, setIsConnected] = useState(false);
  const [deviceName, setDeviceName] = useState(null);
  const [isLive, setIsLive] = useState(false);

  const deviceRef = useRef(null);
  const telemetryCharRef = useRef(null);
  const commandCharRef = useRef(null);
  const rxBufferRef = useRef('');
  const decoderRef = useRef(new TextDecoder('utf-8'));

  const handlePacket = useCallback((jsonStr) => {
    const trimmed = jsonStr.trim();
    if (!trimmed) return;

    try {
      const parsed = JSON.parse(trimmed);
      const reading = normalizeBlePacket(parsed);
      setData(reading);
      setHistory((prev) => [...prev.slice(-(MAX_HISTORY - 1)), reading]);
      setIsLive(true);
      setError(null);
    } catch {
      // Ignore malformed partial frame
    }
  }, []);

  const onNotification = useCallback((event) => {
    const chunk = decoderRef.current.decode(event.target.value);
    rxBufferRef.current += chunk;

    if (rxBufferRef.current.includes('\n')) {
      const lines = rxBufferRef.current.split('\n');
      rxBufferRef.current = lines.pop() || '';
      for (const line of lines) {
        handlePacket(line);
      }
    } else if (rxBufferRef.current.startsWith('{') && rxBufferRef.current.endsWith('}')) {
      const full = rxBufferRef.current;
      rxBufferRef.current = '';
      handlePacket(full);
    }
  }, [handlePacket]);

  const cleanupConnection = useCallback(() => {
    if (telemetryCharRef.current) {
      telemetryCharRef.current.removeEventListener('characteristicvaluechanged', onNotification);
      telemetryCharRef.current = null;
    }
    commandCharRef.current = null;
    rxBufferRef.current = '';
    setIsConnected(false);
    setIsLive(false);
  }, [onNotification]);

  const onDisconnected = useCallback(() => {
    cleanupConnection();
  }, [cleanupConnection]);

  const disconnect = useCallback(() => {
    const device = deviceRef.current;
    if (device) {
      device.removeEventListener('gattserverdisconnected', onDisconnected);
      if (device.gatt?.connected) {
        device.gatt.disconnect();
      }
      deviceRef.current = null;
    }
    cleanupConnection();
    setDeviceName(null);
  }, [cleanupConnection, onDisconnected]);

  const connect = useCallback(async () => {
    if (!bleSupported) {
      setError('Web Bluetooth is not supported in this browser. Open in Chrome, Edge, or Brave.');
      return;
    }

    setError(null);
    setIsConnecting(true);

    try {
      const device = await navigator.bluetooth.requestDevice({
        filters: [
          { services: [BLE_SERVICE_UUID] },
          { namePrefix: 'Wright' },
        ],
        optionalServices: [BLE_SERVICE_UUID],
      });

      if (deviceRef.current && deviceRef.current !== device) {
        deviceRef.current.removeEventListener('gattserverdisconnected', onDisconnected);
        if (deviceRef.current.gatt?.connected) {
          deviceRef.current.gatt.disconnect();
        }
      }

      deviceRef.current = device;
      setDeviceName(device.name || 'Wright-Scale');
      device.addEventListener('gattserverdisconnected', onDisconnected);

      const server = await device.gatt.connect();
      const service = await server.getPrimaryService(BLE_SERVICE_UUID);
      const telemetryChar = await service.getCharacteristic(BLE_TELEMETRY_CHAR_UUID);

      try {
        commandCharRef.current = await service.getCharacteristic(BLE_COMMAND_CHAR_UUID);
      } catch {
        commandCharRef.current = null;
      }

      telemetryCharRef.current = telemetryChar;
      telemetryChar.addEventListener('characteristicvaluechanged', onNotification);
      await telemetryChar.startNotifications();

      setIsConnected(true);
      setIsConnecting(false);

      // Read current value immediately if already populated
      try {
        const initialVal = await telemetryChar.readValue();
        const text = decoderRef.current.decode(initialVal);
        if (text.trim().startsWith('{')) {
          handlePacket(text);
        }
      } catch {
        // Initial read optional; notifications will deliver live packets
      }
    } catch (err) {
      cleanupConnection();
      setIsConnecting(false);
      if (err?.name !== 'NotFoundError') {
        setError(err?.message || 'Bluetooth connection failed');
      }
    }
  }, [bleSupported, cleanupConnection, handlePacket, onDisconnected, onNotification]);

  const tareScale = useCallback(async () => {
    if (!commandCharRef.current) return;
    try {
      const encoder = new TextEncoder();
      await commandCharRef.current.writeValue(encoder.encode('TARE'));
    } catch (err) {
      setError(err?.message || 'Failed to send tare command');
    }
  }, []);

  // Track packet age while connected so stale link status stays accurate
  useEffect(() => {
    if (!isConnected) return undefined;

    const timer = setInterval(() => {
      setData((prev) => {
        if (!prev) return prev;
        const ageMs = Date.now() - prev.receivedAt;
        const isStale = ageMs > STALE_THRESHOLD_MS;
        return {
          ...prev,
          ageMs,
          isStale,
          isOnline: !isStale,
        };
      });
    }, 1000);

    return () => clearInterval(timer);
  }, [isConnected]);

  // Cleanup on unmount
  useEffect(() => {
    return () => {
      disconnect();
    };
  }, [disconnect]);

  return {
    data,
    history,
    error,
    isLoading: isConnecting,
    isLive,
    bleSupported,
    isConnected,
    isConnecting,
    deviceName,
    connect,
    disconnect,
    tareScale,
  };
}

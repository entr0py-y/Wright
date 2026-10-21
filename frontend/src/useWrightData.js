/**
 * Wright — API Client Hook
 * =========================
 * Custom React hook for fetching sensor data from the backend.
 * Polls the latest reading endpoint at ~1 second intervals.
 */

import { useState, useEffect, useCallback, useRef } from 'react';

const API_BASE = import.meta.env.VITE_API_URL || (import.meta.env.PROD ? 'https://wright-y01b.onrender.com' : '');

/**
 * Fetch the latest reading from the backend API.
 * @returns {Object} { data, history, health, error, isLoading, isLive }
 */
export function useWrightData(pollIntervalMs = 1000) {
  const [data, setData] = useState(null);
  const [history, setHistory] = useState([]);
  const [health, setHealth] = useState(null);
  const [error, setError] = useState(null);
  const [isLoading, setIsLoading] = useState(true);
  const [isLive, setIsLive] = useState(false);
  const lastReceivedAt = useRef(null);
  const prevValueRef = useRef(null);
  const [valueChanged, setValueChanged] = useState(false);

  const fetchLatest = useCallback(async () => {
    try {
      const res = await fetch(`${API_BASE}/api/readings/latest`);

      if (res.status === 404) {
        // No readings yet — not an error
        setData(null);
        setError(null);
        setIsLive(false);
        setIsLoading(false);
        return;
      }

      if (!res.ok) {
        throw new Error(`API error: ${res.status} ${res.statusText}`);
      }

      const json = await res.json();

      if (json.isWaiting) {
        setData(null);
        setIsLive(false);
        setError(null);
        setIsLoading(false);
        return;
      }

      // Detect if value actually changed (for animation)
      if (prevValueRef.current) {
        const changed = prevValueRef.current.weightKg !== json.weightKg ||
          prevValueRef.current.heightCm !== json.heightCm;
        setValueChanged(changed);
        if (changed) {
          setTimeout(() => setValueChanged(false), 200);
        }
      }
      prevValueRef.current = json;

      setData(json);
      setIsLive(json.isOnline || false);
      setError(null);
      setIsLoading(false);
      lastReceivedAt.current = Date.now();
    } catch (err) {
      setError(err.message);
      setIsLive(false);
      setIsLoading(false);
    }
  }, []);

  const fetchHistory = useCallback(async () => {
    try {
      const res = await fetch(`${API_BASE}/api/readings/history?limit=50`);
      if (res.ok) {
        const json = await res.json();
        setHistory(json.readings || []);
      }
    } catch {
      // History fetch failure is non-critical
    }
  }, []);

  const fetchHealth = useCallback(async () => {
    try {
      const res = await fetch(`${API_BASE}/api/health`);
      if (res.ok) {
        setHealth(await res.json());
      } else {
        setHealth(null);
      }
    } catch {
      setHealth(null);
    }
  }, []);

  useEffect(() => {
    // Initial fetch
    fetchLatest();
    fetchHistory();
    fetchHealth();

    // Poll latest every second
    const latestInterval = setInterval(fetchLatest, pollIntervalMs);

    // Poll history every 5 seconds
    const historyInterval = setInterval(fetchHistory, 5000);

    // Poll health every 10 seconds
    const healthInterval = setInterval(fetchHealth, 10000);

    return () => {
      clearInterval(latestInterval);
      clearInterval(historyInterval);
      clearInterval(healthInterval);
    };
  }, [fetchLatest, fetchHistory, fetchHealth, pollIntervalMs]);

  return { data, history, health, error, isLoading, isLive, valueChanged };
}

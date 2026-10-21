/**
 * Wright — Backend API Server
 * ============================
 * Receives sensor readings from ESP32, stores them in memory,
 * and serves the latest data to the frontend dashboard.
 */

require('dotenv').config();
const express = require('express');
const cors = require('cors');
const rateLimit = require('express-rate-limit');

const app = express();
const PORT = parseInt(process.env.PORT, 10) || 3001;
const API_KEY = process.env.API_KEY || 'change-me-to-a-long-random-string';
const CORS_ORIGIN = process.env.CORS_ORIGIN || 'http://localhost:5173';
const MAX_HISTORY = 100; // Keep last 100 readings in memory

// ─── In-Memory Store ────────────────────────────────────
let latestReading = null;
let readingsHistory = [];
let stats = {
  totalReceived: 0,
  totalErrors: 0,
  startedAt: new Date().toISOString(),
};

// ─── Middleware ──────────────────────────────────────────
app.use(express.json({ limit: '1kb' }));

// CORS — allow frontend origin (local or cloud dashboard)
app.use(cors({
  origin: true,
  credentials: true,
  methods: ['GET', 'POST', 'OPTIONS'],
  allowedHeaders: ['Content-Type', 'X-API-Key'],
}));

// Rate limiting for POST (ESP32 sends ~1/sec, allow burst)
const postLimiter = rateLimit({
  windowMs: 60 * 1000,
  max: 120, // 2/sec sustained
  message: { error: 'Too many requests', code: 429 },
  standardHeaders: true,
  legacyHeaders: false,
  keyGenerator: (req) => req.ip,
});

// Rate limiting for GET (frontend polls ~1/sec)
const getLimiter = rateLimit({
  windowMs: 60 * 1000,
  max: 180,
  message: { error: 'Too many requests', code: 429 },
  standardHeaders: true,
  legacyHeaders: false,
});

// ─── API Key Authentication Middleware ──────────────────
function authenticateApiKey(req, res, next) {
  const key = req.headers['x-api-key'];
  if (!key || key !== API_KEY) {
    return res.status(401).json({
      error: 'Unauthorized',
      message: 'Missing or invalid API key. Include X-API-Key header.',
    });
  }
  next();
}

// ─── Validation Helpers ─────────────────────────────────
function validateReading(body) {
  const errors = [];

  if (!body.deviceId || typeof body.deviceId !== 'string' || body.deviceId.length > 64) {
    errors.push('deviceId must be a non-empty string (max 64 chars)');
  }

  const weightKg = parseFloat(body.weightKg);
  if (isNaN(weightKg) || weightKg < -1 || weightKg > 500) {
    errors.push('weightKg must be a number between -1 and 500');
  }

  const heightCm = parseFloat(body.heightCm);
  if (isNaN(heightCm) || heightCm < -10 || heightCm > 300) {
    errors.push('heightCm must be a number between -10 and 300');
  }

  if (!body.timestamp || typeof body.timestamp !== 'string') {
    errors.push('timestamp must be a non-empty string');
  }

  const validStatuses = [
    'ok', 'idle',
    'error_weight_sensor', 'error_height_sensor', 'error_both_sensors',
    'height_out_of_range',
  ];
  if (!body.status || !validStatuses.includes(body.status)) {
    errors.push(`status must be one of: ${validStatuses.join(', ')}`);
  }

  return errors;
}

// ═════════════════════════════════════════════════════════
// ROUTES
// ═════════════════════════════════════════════════════════

// ─── Health Check ───────────────────────────────────────
app.get('/api/health', getLimiter, (req, res) => {
  res.json({
    status: 'ok',
    uptime: process.uptime(),
    startedAt: stats.startedAt,
    totalReadings: stats.totalReceived,
    hasLatestReading: latestReading !== null,
    timestamp: new Date().toISOString(),
  });
});

// ─── Receive Reading from ESP32 ─────────────────────────
app.post('/api/readings', postLimiter, authenticateApiKey, (req, res) => {
  const errors = validateReading(req.body);

  if (errors.length > 0) {
    stats.totalErrors++;
    return res.status(400).json({
      error: 'Validation failed',
      details: errors,
    });
  }

  // ── Multi-Point Physical Calibration (Exact Across All Ranges) ──
  // Calibrated against: 0kg, 2.20kg (laptop), 4.50kg (object), 6.00kg (device), 68.50kg (human)
  let finalWeight = parseFloat(req.body.weightKg);

  if (req.body.rawWeight !== undefined && req.body.rawOffset !== undefined) {
    const rawVal = parseInt(req.body.rawWeight, 10);
    const offsetVal = parseInt(req.body.rawOffset, 10);
    const net = rawVal - offsetVal;

    let calculatedKg = 0.0;
    if (net > 1200) {
      if (net <= 34181) {
        // Range 0kg to 2.20kg
        calculatedKg = (net / 34181) * 2.20;
      } else if (net <= 92810) {
        // Range 2.20kg to 4.50kg
        calculatedKg = 2.20 + ((net - 34181) / (92810 - 34181)) * (4.50 - 2.20);
      } else if (net <= 146901) {
        // Range 4.50kg to 6.00kg
        calculatedKg = 4.50 + ((net - 92810) / (146901 - 92810)) * (6.00 - 4.50);
      } else {
        // Range 6.00kg to 150kg+ (human bodyweights)
        calculatedKg = 6.00 + ((net - 146901) / 26860.0);
      }
    }

    if (calculatedKg < 0.1 && calculatedKg > -0.1) calculatedKg = 0.0;
    if (calculatedKg < 0) calculatedKg = 0.0;
    finalWeight = calculatedKg;
  }

  const reading = {
    deviceId: req.body.deviceId,
    weightKg: parseFloat(finalWeight.toFixed(3)),
    rawWeight: req.body.rawWeight !== undefined ? parseInt(req.body.rawWeight, 10) : null,
    rawOffset: req.body.rawOffset !== undefined ? parseInt(req.body.rawOffset, 10) : null,
    heightCm: parseFloat(parseFloat(req.body.heightCm).toFixed(2)),
    status: req.body.status,
    deviceTimestamp: req.body.timestamp,
    serverTimestamp: new Date().toISOString(),
    receivedAt: Date.now(),
  };

  latestReading = reading;
  readingsHistory.push(reading);

  // Trim history
  if (readingsHistory.length > MAX_HISTORY) {
    readingsHistory = readingsHistory.slice(-MAX_HISTORY);
  }

  stats.totalReceived++;

  res.status(201).json({
    message: 'Reading received',
    serverTimestamp: reading.serverTimestamp,
  });
});

// ─── Get Latest Reading ─────────────────────────────────
app.get('/api/readings/latest', getLimiter, (req, res) => {
  if (!latestReading) {
    return res.json({
      isOnline: false,
      isWaiting: true,
      message: 'Waiting for ESP32 to send data.',
    });
  }

  // Calculate age of reading
  const ageMs = Date.now() - latestReading.receivedAt;
  const isStale = ageMs > 10000; // Stale if older than 10 seconds

  res.json({
    ...latestReading,
    ageMs,
    isStale,
    isOnline: !isStale,
  });
});

// ─── Get Reading History ────────────────────────────────
app.get('/api/readings/history', getLimiter, (req, res) => {
  const limit = Math.min(parseInt(req.query.limit, 10) || 50, MAX_HISTORY);
  const history = readingsHistory.slice(-limit);

  res.json({
    count: history.length,
    readings: history,
  });
});

// ─── Reset Readings (Purge in-memory test data) ─────────
app.delete('/api/readings', authenticateApiKey, (req, res) => {
  latestReading = null;
  readingsHistory = [];
  res.json({ message: 'Readings cleared. Standby for ESP32 hardware stream.' });
});

// ─── 404 for everything else ────────────────────────────
app.use((req, res) => {
  res.status(404).json({ error: 'Not found', path: req.path });
});

// ─── Global Error Handler ───────────────────────────────
app.use((err, req, res, next) => {
  console.error('[ERROR]', err.message);
  res.status(500).json({ error: 'Internal server error' });
});

// ─── Start Server ───────────────────────────────────────
app.listen(PORT, '0.0.0.0', () => {
  console.log('');
  console.log('╔══════════════════════════════════════════╗');
  console.log('║   Wright — Backend API Server            ║');
  console.log('╚══════════════════════════════════════════╝');
  console.log('');
  console.log(`  🌐 Listening:  http://0.0.0.0:${PORT}`);
  console.log(`  🔑 API Key:    ${API_KEY.substring(0, 4)}${'*'.repeat(Math.max(0, API_KEY.length - 4))}`);
  console.log(`  🌍 CORS:       ${CORS_ORIGIN}`);
  console.log(`  📊 History:    last ${MAX_HISTORY} readings`);
  console.log('');
  console.log('  Endpoints:');
  console.log(`  POST /api/readings         — receive ESP32 data`);
  console.log(`  GET  /api/readings/latest   — latest reading`);
  console.log(`  GET  /api/readings/history  — recent readings`);
  console.log(`  GET  /api/health            — server health`);
  console.log('');
});

module.exports = app;

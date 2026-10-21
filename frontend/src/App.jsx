/**
 * Wright — Smart Weighing Machine Interface
 * ==========================================
 * Strict Duo-Color (Black & White) Industrial Layout.
 * No tiles, no gradients, no artificial cards.
 */

import { useMemo } from 'react';
import './App.css';
import { useWrightData } from './useWrightData';

// ─── Helpers ────────────────────────────────────────────
function formatTime(isoString) {
  if (!isoString) return '—';
  const d = new Date(isoString);
  return d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit', hour12: false });
}

function formatTimeAgo(isoString) {
  if (!isoString) return 'NEVER';
  const seconds = Math.floor((Date.now() - new Date(isoString).getTime()) / 1000);
  if (seconds < 2) return 'JUST NOW';
  if (seconds < 60) return `${seconds}S AGO`;
  if (seconds < 3600) return `${Math.floor(seconds / 60)}M AGO`;
  return `${Math.floor(seconds / 3600)}H AGO`;
}

function getStatusLabel(status) {
  switch (status) {
    case 'ok': return 'OPERATIONAL';
    case 'idle': return 'IDLE / ZERO';
    case 'error_weight_sensor': return 'ERR: HX711 FAULT';
    case 'error_height_sensor': return 'ERR: HC-SR04 FAULT';
    case 'error_both_sensors': return 'ERR: MULTI-SENSOR';
    case 'height_out_of_range': return 'WARN: OUT OF RANGE';
    default: return status ? status.toUpperCase() : 'UNKNOWN';
  }
}

// ─── Monochrome Minimal Chart ───────────────────────────
function MiniChart({ history, isEspOnline }) {
  const chartData = useMemo(() => {
    if (!isEspOnline || !history || history.length < 2) return null;

    const readings = history.slice(-30);
    const padding = { top: 20, right: 10, bottom: 25, left: 55 };
    const width = 800;
    const height = 180;
    const innerW = width - padding.left - padding.right;
    const innerH = height - padding.top - padding.bottom;

    const weights = readings.map(r => r.weightKg).filter(v => v >= 0);
    const heights = readings.map(r => r.heightCm).filter(v => v >= 0);

    const wMin = Math.min(...weights, 0);
    const wMax = Math.max(...weights, 1);
    const hMin = Math.min(...heights, 0);
    const hMax = Math.max(...heights, 1);

    const scaleX = (i) => padding.left + (i / (readings.length - 1)) * innerW;
    const scaleW = (v) => padding.top + innerH - ((v - wMin) / (wMax - wMin || 1)) * innerH;
    const scaleH = (v) => padding.top + innerH - ((v - hMin) / (hMax - hMin || 1)) * innerH;

    let weightLine = '';
    let heightLine = '';

    readings.forEach((r, i) => {
      const x = scaleX(i);
      if (r.weightKg >= 0) {
        const y = scaleW(r.weightKg);
        weightLine += (weightLine ? ` L${x},${y}` : `M${x},${y}`);
      }
      if (r.heightCm >= 0) {
        const y = scaleH(r.heightCm);
        heightLine += (heightLine ? ` L${x},${y}` : `M${x},${y}`);
      }
    });

    const gridLines = [0, 0.5, 1].map(frac => ({
      y: padding.top + (1 - frac) * innerH,
      label: (wMin + frac * (wMax - wMin)).toFixed(1),
    }));

    const timeLabels = [0, Math.floor(readings.length / 2), readings.length - 1]
      .filter(i => i < readings.length)
      .map(i => ({
        x: scaleX(i),
        label: formatTime(readings[i].serverTimestamp),
      }));

    return { weightLine, heightLine, gridLines, timeLabels, width, height, padding };
  }, [history, isEspOnline]);

  if (!chartData) {
    return (
      <div className="chart-empty">
        {isEspOnline
          ? '[ ACCUMULATING REAL-TIME SAMPLES… ]'
          : '[ STANDBY — AWAITING ESP32 LINK · NO DUMMY DATA ]'}
      </div>
    );
  }

  return (
    <svg className="chart-svg" viewBox={`0 0 ${chartData.width} ${chartData.height}`} preserveAspectRatio="none">
      {/* Hairline Grid Lines */}
      {chartData.gridLines.map((g, i) => (
        <g key={i}>
          <line className="chart-grid" x1={chartData.padding.left} y1={g.y} x2={chartData.width - chartData.padding.right} y2={g.y} />
          <text className="chart-axis-text" x={chartData.padding.left - 8} y={g.y + 3} textAnchor="end">{g.label}k</text>
        </g>
      ))}

      {/* Time Markers */}
      {chartData.timeLabels.map((t, i) => (
        <text key={i} className="chart-axis-text" x={t.x} y={chartData.height - 4} textAnchor="middle">{t.label}</text>
      ))}

      {/* Raw Hairline Data Paths (Pure White Solid for Weight, Dashed for Height) */}
      {chartData.heightLine && <path className="chart-trace-height" d={chartData.heightLine} />}
      {chartData.weightLine && <path className="chart-trace-weight" d={chartData.weightLine} />}
    </svg>
  );
}

// ─── Main Interface ─────────────────────────────────────
export default function App() {
  const { data, history, health, error, isLoading, isLive } = useWrightData(1000);

  const isEspOnline = Boolean(data && !data.isStale && data.isOnline);

  const deviceStatus = useMemo(() => {
    if (isLoading && !data) return 'CONNECTING';
    if (error) return 'DISCONNECTED';
    if (!data) return 'OFFLINE';
    if (data.isStale) return 'STANDBY';
    return 'ONLINE';
  }, [isLoading, error, data]);

  // Strict: ONLY show values when real ESP32 hardware is actively streaming
  const weightVal = (isEspOnline && data.weightKg !== undefined && data.weightKg >= 0)
    ? data.weightKg.toFixed(3)
    : null;

  const heightVal = (isEspOnline && data.heightCm !== undefined && data.heightCm >= 0)
    ? data.heightCm.toFixed(2)
    : null;

  return (
    <div className="terminal">
      {/* ── Top Industrial Status Header ── */}
      <header className="terminal-header">
        <div className="terminal-brand">
          <span className="brand-title">WRIGHT</span>
          <span className="brand-dot">/</span>
          <span className="brand-sub">SCALE SYSTEM 01</span>
        </div>
        <div className="terminal-meta">
          <span className="meta-item">MODE: DUAL-SENSOR</span>
          <span className="meta-separator">|</span>
          <span className={`meta-status ${deviceStatus === 'ONLINE' ? 'meta-status--active' : ''}`}>
            [{deviceStatus}]
          </span>
          <span className="meta-separator">|</span>
          <span className="meta-item">{isLive ? 'STREAM: LIVE' : 'STREAM: PAUSED'}</span>
        </div>
      </header>

      {/* ── Critical Alerts (High-Contrast Inverted Banner) ── */}
      {error && (
        <div className="terminal-banner">
          [SYSTEM ALERT] BACKEND CONNECTION INTERRUPTED: {error}
        </div>
      )}

      {/* ── Primary Readouts: Pure Typographic Scale, NO TILES ── */}
      <main className="readouts-grid">
        {/* Weight Readout */}
        <div className="readout-block" id="readout-weight">
          <div className="readout-caption">
            <span className="caption-num">01</span>
            <span className="caption-name">LOAD MASS / WEIGHT</span>
            <span className="caption-diag">
              {data ? getStatusLabel(data.status) : 'OFFLINE'}
            </span>
          </div>

          <div className="readout-display">
            {weightVal !== null ? (
              <div className="display-row">
                <span className="display-number">{weightVal}</span>
                <span className="display-unit">KG</span>
              </div>
            ) : (
              <div className="display-row">
                <span className="display-number display-number--standby">— — —</span>
                <span className="display-unit">STANDBY</span>
              </div>
            )}
          </div>

          <div className="readout-alt">
            {weightVal !== null
              ? `EQUIV: ${(parseFloat(weightVal) * 2.20462).toFixed(3)} LBS`
              : (isEspOnline ? 'AWAITING PLATFORM LOAD' : 'POWER ON ESP32 TO STREAM REAL SENSOR READINGS')}
          </div>
        </div>

        {/* Height Readout */}
        <div className="readout-block" id="readout-height">
          <div className="readout-caption">
            <span className="caption-num">02</span>
            <span className="caption-name">STATURE / HEIGHT</span>
            <span className="caption-diag">
              {isEspOnline
                ? (data?.status === 'height_out_of_range' ? 'OUT OF RANGE' :
                   data?.status === 'error_height_sensor' ? 'ECHO TIMEOUT' : 'OPERATIONAL')
                : 'OFFLINE'}
            </span>
          </div>

          <div className="readout-display">
            {heightVal !== null ? (
              <div className="display-row">
                <span className="display-number">{heightVal}</span>
                <span className="display-unit">CM</span>
              </div>
            ) : (
              <div className="display-row">
                <span className="display-number display-number--standby">— — —</span>
                <span className="display-unit">STANDBY</span>
              </div>
            )}
          </div>

          <div className="readout-alt">
            {heightVal !== null
              ? `EQUIV: ${(parseFloat(heightVal) / 100).toFixed(4)} METERS`
              : (isEspOnline ? 'AWAITING ULTRASONIC TARGET' : 'POWER ON ESP32 TO STREAM REAL SENSOR READINGS')}
          </div>
        </div>
      </main>

      {/* ── Telemetry Specification Sheet (Linear Row, NO TILES) ── */}
      <section className="spec-section">
        <div className="spec-heading">
          <span>HARDWARE TELEMETRY & LINK STATUS</span>
          <span>POLL_INTERVAL: 1000MS</span>
        </div>
        <div className="spec-table">
          <div className="spec-col">
            <span className="spec-key">DEVICE IDENTIFIER</span>
            <span className="spec-val">{isEspOnline ? (data?.deviceId || '—') : '[STANDBY / OFFLINE]'}</span>
          </div>
          <div className="spec-col">
            <span className="spec-key">ESP32 UPTIME</span>
            <span className="spec-val">{isEspOnline ? (data?.deviceTimestamp || '—') : 'DISCONNECTED'}</span>
          </div>
          <div className="spec-col">
            <span className="spec-key">LAST PACKET SYNC</span>
            <span className="spec-val">{isEspOnline ? formatTimeAgo(data.serverTimestamp) : 'AWAITING LINK'}</span>
          </div>
          <div className="spec-col">
            <span className="spec-key">SERVER TIMESTAMP</span>
            <span className="spec-val">{isEspOnline ? formatTime(data.serverTimestamp) : 'AWAITING STREAM'}</span>
          </div>
          <div className="spec-col">
            <span className="spec-key">BACKEND LINK</span>
            <span className="spec-val">{health ? `ACTIVE [PORT 3001]` : 'OFFLINE'}</span>
          </div>
          <div className="spec-col">
            <span className="spec-key">DATA AGE / STATUS</span>
            <span className="spec-val">
              {isEspOnline ? `${(data.ageMs / 1000).toFixed(1)}S [LIVE]` : 'STANDBY (NO SIGNAL)'}
            </span>
          </div>
        </div>
      </section>

      {/* ── Waveform Trace: Minimal 1px Hairline Monochrome ── */}
      <section className="trace-section">
        <div className="trace-heading">
          <span>TEMPORAL SIGNAL CHRONOLOGY</span>
          <div className="trace-legend">
            <span className="legend-entry">
              <span className="legend-line legend-line--solid"></span> WEIGHT (KG)
            </span>
            <span className="legend-entry">
              <span className="legend-line legend-line--dashed"></span> HEIGHT (CM)
            </span>
          </div>
        </div>
        <div className="trace-canvas">
          <MiniChart history={history} isEspOnline={isEspOnline} />
        </div>
      </section>

      {/* ── Minimal Terminal Footer ── */}
      <footer className="terminal-footer">
        <span>WRIGHT INSTRUMENTATION / PROTOCOL V2.0</span>
        <span>ESP32-WROOM / HX711 / HC-SR04</span>
      </footer>
    </div>
  );
}

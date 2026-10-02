/**
 * MUTEq Dashboard — TradingView Lightweight Charts
 *
 * Features:
 *  - SPL area chart; initial view = last 2h, scroll back freely
 *  - Live mode: toggleable rolling 30-min window that tracks new data
 *  - Auto-refresh every 30 s: appends new readings and refreshes stats
 *  - CSS bar charts for noise distribution histogram + hourly event heatmap
 *  - KPI cards, daily stats table, threshold events table
 */

'use strict';

// ── Constants ────────────────────────────────────────────────────────────────

const BATCH_SIZE            = 2000;
const PREFETCH_BARS         = 50;           // trigger history-load this many bars from left edge
const REFRESH_INTERVAL_MS   = 30_000;
const INITIAL_WINDOW_SECONDS = 2 * 60 * 60; // 2 h shown on first load
const LIVE_WINDOW_SECONDS    = 30 * 60;      // 30 min rolling window in live mode
const STATS_WINDOW_SECONDS   = 24 * 60 * 60; // 24 h window for KPI / events panel

// ── State ────────────────────────────────────────────────────────────────────

let chart          = null;
let splSeries      = null;
let priceLinesAdded = false;
let liveMode       = false;

let loadedData    = [];   // [{time: unix_seconds, value: dB}], sorted ascending
let isLoading     = false;
let noMoreHistory = false;
let refreshTimer  = null;

// ── DOM helpers ──────────────────────────────────────────────────────────────

const $ = (id) => document.getElementById(id);

function showLoading(on) {
  $('loading-overlay').classList.toggle('hidden', !on);
}

function showEmpty(on) {
  $('empty-state').classList.toggle('hidden', !on);
}

function setStatus(text, type = 'ok') {
  const pill = $('status-pill');
  pill.textContent = text;
  pill.className = `pill pill--${type}`;
}

// ── Chart initialisation ─────────────────────────────────────────────────────

function createChart() {
  const container = $('chart-container');

  chart = LightweightCharts.createChart(container, {
    width:  container.clientWidth,
    height: container.clientHeight,
    localization: {
      timeFormatter: (ts) => new Date(ts * 1000).toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' }),
    },
    layout: {
      background: { color: '#131722' },
      textColor: '#d1d4dc',
    },
    grid: {
      vertLines: { color: '#1e2130' },
      horzLines: { color: '#1e2130' },
    },
    crosshair: {
      mode: LightweightCharts.CrosshairMode.Normal,
      vertLine: { color: '#758696', labelBackgroundColor: '#2a2e3e' },
      horzLine: { color: '#758696', labelBackgroundColor: '#2a2e3e' },
    },
    rightPriceScale: {
      borderColor: '#2a2e3e',
      textColor: '#d1d4dc',
    },
    timeScale: {
      borderColor: '#2a2e3e',
      timeVisible: true,
      secondsVisible: false,
      rightOffset: 12,
      // Render all axis tick labels in the browser's local timezone
      tickMarkFormatter: (ts, tickMarkType) => {
        const d = new Date(ts * 1000);
        const TT = LightweightCharts.TickMarkType;
        switch (tickMarkType) {
          case TT.Year:        return d.toLocaleDateString([], { year: 'numeric' });
          case TT.Month:       return d.toLocaleDateString([], { month: 'short', year: 'numeric' });
          case TT.DayOfMonth:  return d.toLocaleDateString([], { month: 'short', day: 'numeric' });
          case TT.Time:        return d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
          case TT.TimeWithSeconds: return d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' });
          default:             return d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit' });
        }
      },
    },
    handleScroll: { mouseWheel: true, pressedMouseMove: true },
    handleScale:  { mouseWheel: true, pinch: true },
  });

  splSeries = chart.addAreaSeries({
    topColor:               'rgba(56, 189, 248, 0.28)',
    bottomColor:            'rgba(56, 189, 248, 0.02)',
    lineColor:              '#38bdf8',
    lineWidth:              2,
    priceLineVisible:       false,
    lastValueVisible:       true,
    crosshairMarkerVisible: true,
    crosshairMarkerRadius:  4,
    priceFormat: { type: 'price', precision: 1, minMove: 0.1 },
  });

  chart.subscribeCrosshairMove(updateTooltip);

  new ResizeObserver(() => {
    chart.applyOptions({
      width:  container.clientWidth,
      height: container.clientHeight,
    });
  }).observe(container);

  chart.timeScale().subscribeVisibleLogicalRangeChange(onRangeChange);
}

function updateTooltip(param) {
  const tooltip = $('spl-tooltip');
  if (!param || !param.time || !splSeries) {
    tooltip.classList.add('hidden');
    return;
  }
  const bar = param.seriesData && param.seriesData.get(splSeries);
  if (!bar) {
    tooltip.classList.add('hidden');
    return;
  }

  const dt    = new Date(bar.time * 1000);
  const color = bar.value >= 90 ? '#f87171' : bar.value >= 70 ? '#facc15' : '#4ade80';

  tooltip.innerHTML =
    `<span class="tt-time">${dt.toLocaleString()}</span>` +
    `<span class="tt-val" style="color:${color}">${bar.value.toFixed(1)} dB</span>`;
  tooltip.classList.remove('hidden');
}

// ── Live mode ─────────────────────────────────────────────────────────────────

function applyLiveRange() {
  const now = Math.floor(Date.now() / 1000);
  chart.timeScale().setVisibleRange({
    from: now - LIVE_WINDOW_SECONDS,
    to:   now,
  });
}

function toggleLive() {
  liveMode = !liveMode;
  $('live-btn').classList.toggle('active', liveMode);
  if (liveMode) applyLiveRange();
}

// ── API ───────────────────────────────────────────────────────────────────────

async function apiFetch(path) {
  const res = await fetch(path);
  if (!res.ok) throw new Error(`${path} → ${res.status}`);
  return res.json();
}

async function fetchReadings(fromTs = null, toTs = null, limit = BATCH_SIZE) {
  const p = new URLSearchParams({ limit });
  if (fromTs !== null) p.set('from_ts', fromTs);
  if (toTs   !== null) p.set('to_ts',   toTs);
  return apiFetch(`/api/readings?${p}`);
}

async function fetchEvents(fromTs = null) {
  const p = new URLSearchParams({ limit: 500 });
  if (fromTs !== null) p.set('from_ts', fromTs);
  return apiFetch(`/api/events?${p}`);
}

async function fetchStats(fromTs = null) {
  const p = new URLSearchParams();
  if (fromTs !== null) p.set('from_ts', fromTs);
  return apiFetch(`/api/stats?${p}`);
}

// ── Data management ───────────────────────────────────────────────────────────

function mergeData(newData) {
  if (!newData.length) return;
  if (!loadedData.length) {
    loadedData = newData;
    return;
  }
  const times   = new Set(loadedData.map((d) => d.time));
  const deduped = newData.filter((d) => !times.has(d.time));
  if (!deduped.length) return;

  if (deduped[0].time < loadedData[0].time) {
    loadedData = [...deduped, ...loadedData];
  } else {
    loadedData = [...loadedData, ...deduped];
  }
}

function applyDataToChart() {
  if (!splSeries) return;
  // Deduplicate by time (keep last per second) and drop null/NaN values
  const seen = new Map();
  for (const d of loadedData) {
    if (d.value != null && isFinite(d.value)) seen.set(d.time, d);
  }
  const clean = Array.from(seen.values()).sort((a, b) => a.time - b.time);
  splSeries.setData(clean);
  if (!priceLinesAdded && clean.length > 0) {
    splSeries.createPriceLine({
      price: 60, color: '#4ade80', lineWidth: 1,
      lineStyle: LightweightCharts.LineStyle.Dashed,
      axisLabelVisible: true, title: '60 dB',
    });
    splSeries.createPriceLine({
      price: 70, color: '#f87171', lineWidth: 1,
      lineStyle: LightweightCharts.LineStyle.Dashed,
      axisLabelVisible: true, title: '70 dB',
    });
    priceLinesAdded = true;
  }
}

// ── Load / reload ─────────────────────────────────────────────────────────────

async function loadInitial() {
  if (isLoading) return;
  isLoading     = true;
  noMoreHistory = false;
  loadedData    = [];

  showLoading(true);
  showEmpty(false);
  setStatus('Loading\u2026', 'loading');

  const fromTs = Math.floor(Date.now() / 1000) - INITIAL_WINDOW_SECONDS;

  try {
    const data = await fetchReadings(fromTs, null, BATCH_SIZE);
    if (!data.length) {
      showEmpty(true);
      setStatus('No data', 'warn');
      return;
    }
    mergeData(data);
    applyDataToChart();
    if (liveMode) {
      applyLiveRange();
    } else {
      chart.timeScale().scrollToRealTime();
    }
    setStatus('Live', 'ok');
    markUpdated();
    await refreshStats();
  } catch (err) {
    console.error('loadInitial error:', err);
    setStatus('Error', 'error');
  } finally {
    showLoading(false);
    isLoading = false;
  }
}

/** Fetch older data when the user scrolls left. */
async function loadOlderHistory() {
  if (isLoading || noMoreHistory || !loadedData.length) return;
  isLoading = true;

  const oldestTime = loadedData[0].time;
  try {
    const data = await fetchReadings(null, oldestTime - 1, BATCH_SIZE);
    if (!data.length) {
      noMoreHistory = true;
      return;
    }
    mergeData(data);
    applyDataToChart();
  } catch (err) {
    console.error('loadOlderHistory error:', err);
  } finally {
    isLoading = false;
  }
}

/** Poll for new readings and refresh stats every 30 s. */
async function refreshLatest() {
  if (isLoading) return;
  // If initial load found no data, retry a full load instead
  if (!loadedData.length) { await loadInitial(); return; }

  const latestTime = loadedData[loadedData.length - 1].time;

  try {
    const data = await fetchReadings(latestTime + 1, null, BATCH_SIZE);
    if (data.length) {
      mergeData(data);
      applyDataToChart();
      markUpdated();
    }
    if (liveMode) applyLiveRange();
    await refreshStats();
    setStatus('Live', 'ok');
  } catch (err) {
    console.error('refreshLatest error:', err);
    setStatus('Error', 'error');
  }
}

async function refreshStats() {
  const fromTs = Math.floor(Date.now() / 1000) - STATS_WINDOW_SECONDS;
  try {
    const [stats, events] = await Promise.all([
      fetchStats(fromTs),
      fetchEvents(fromTs),
    ]);
    updateKpi(stats);
    renderHistogram(stats.histogram);
    renderHeatmap(stats.heatmap);
    renderDailyTable(stats.daily_stats);
    renderEventsTable(events);
  } catch (err) {
    console.error('refreshStats error:', err);
  }
}

function onRangeChange(range) {
  if (!range || isLoading || noMoreHistory || !loadedData.length) return;
  if (range.from <= PREFETCH_BARS) {
    loadOlderHistory();
  }
}

function markUpdated() {
  $('last-updated').textContent = `Updated ${new Date().toLocaleTimeString()}`;
}

// ── KPI cards ─────────────────────────────────────────────────────────────────

function updateKpi(stats) {
  const fmt = (v) => (v != null ? v.toFixed(1) : '\u2014');

  $('kpi-latest').textContent = fmt(stats.latest);
  $('kpi-peak').textContent   = fmt(stats.peak);
  $('kpi-events').textContent = stats.event_count;
  $('kpi-p90').textContent    = fmt(stats.percentiles.p90);

  const v = stats.latest;
  $('kpi-latest').className =
    'card-value' + (v == null ? '' : v >= 90 ? ' alert' : v >= 70 ? ' warn' : ' ok');
}

// ── Bar charts ────────────────────────────────────────────────────────────────

function renderHistogram(hist) {
  const max = Math.max(...hist.counts, 1);
  $('histogram-chart').innerHTML = hist.labels
    .map((label, i) => {
      const pct   = Math.round((hist.counts[i] / max) * 100);
      const frac  = i / (hist.labels.length - 1);
      const color = frac < 0.5 ? '#4ade80' : frac < 0.75 ? '#facc15' : '#f87171';
      return barRow(label, pct, color, hist.counts[i]);
    })
    .join('');
}

function renderHeatmap(heatmap) {
  const max = Math.max(...heatmap, 1);
  $('heatmap-chart').innerHTML = heatmap
    .map((count, hour) => {
      const pct   = Math.round((count / max) * 100);
      const color = count === 0 ? '#1e3a5f' : '#f87171';
      const label = String(hour).padStart(2, '0') + ':00';
      return barRow(label, pct, color, count);
    })
    .join('');
}

function barRow(label, pct, color, count) {
  return (
    `<div class="bar-row">` +
    `<span class="bar-label">${label}</span>` +
    `<div class="bar-track"><div class="bar-fill" style="width:${pct}%;background:${color}"></div></div>` +
    `<span class="bar-count">${count}</span>` +
    `</div>`
  );
}

// ── Tables ────────────────────────────────────────────────────────────────────

function renderDailyTable(daily) {
  const tbody = $('daily-tbody');
  if (!daily.length) {
    tbody.innerHTML = '<tr><td colspan="4">No data yet.</td></tr>';
    return;
  }
  tbody.innerHTML = daily
    .map(
      (d) =>
        `<tr><td>${d.day}</td><td>${d.avg_noise ?? '\u2014'}</td>` +
        `<td>${d.peak_noise ?? '\u2014'}</td><td>${d.event_count}</td></tr>`
    )
    .join('');
}

function renderEventsTable(events) {
  const tbody = $('events-tbody');
  if (!events.length) {
    tbody.innerHTML = '<tr><td colspan="3">No events in the last 24h.</td></tr>';
    return;
  }
  tbody.innerHTML = events
    .map(
      (e) =>
        `<tr><td>${new Date(e.time * 1000).toLocaleString()}</td>` +
        `<td>${e.noise.toFixed(1)}</td>` +
        `<td>${e.peak != null ? e.peak.toFixed(1) : '\u2014'}</td></tr>`
    )
    .join('');
}

// ── Boot ─────────────────────────────────────────────────────────────────────

async function boot() {
  createChart();

  try {
    const cfg = await apiFetch('/api/config');
    document.title = `${cfg.device_name} \u2014 MUTEq`;
    $('device-name').textContent = cfg.device_name;
    if (cfg.location)            $('location').textContent    = `\u{1F4CD} ${cfg.location}`;
    if (cfg.environment_profile) $('env-profile').textContent = cfg.environment_profile;
  } catch (err) {
    console.warn('Could not load config:', err);
  }

  $('live-btn').addEventListener('click', toggleLive);

  await loadInitial();

  refreshTimer = setInterval(refreshLatest, REFRESH_INTERVAL_MS);
}

document.addEventListener('DOMContentLoaded', boot);

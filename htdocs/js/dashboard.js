/**
 * dashboard.js — PMO frontend application logic
 *
 * UI ARCHITECTURE
 * ---------------
 * All pages share this single script. Functions that reference DOM elements
 * that don't exist on the current page simply no-op (guard clauses check with
 * getElementById before writing).
 *
 * DATA FLOW
 * ---------
 *   fetchData() polls data.php every 1 second
 *     → updateCards()    renders all metric values and relay status
 *     → checkStaleness() detects when the device has stopped sending data
 *     → seedChart()      (first call only) bulk-loads 50-row history into charts
 *     → pushPoint()      (subsequent calls) appends the newest reading
 *     → rebuildTable()   rewrites the history table rows
 *     → runDetection()   triggers NILM inference (throttled to every 4 s)
 *
 * STALE DETECTION
 * ---------------
 * The ESP32 posts every 3 seconds, so data.php should return a new row_id on
 * most 1-second polls. If the same id is seen 5 times in a row the connection
 * is considered stale: metric cards are greyed, relay buttons are disabled,
 * and the status indicator switches to "Stale". The state clears automatically
 * the next time a new id arrives.
 *
 * CHART MANAGEMENT
 * ----------------
 * Charts maintain a 60-point sliding window. seedChart() initialises them from
 * the 50-row history on the first successful fetch. pushPoint() then appends
 * one point per new reading and shifts off the oldest when the window is full.
 * chart.update('none') skips the animation on each incremental update for a
 * smooth live-data feel.
 *
 * NILM REGISTRATION WORKFLOW
 * --------------------------
 * 1. User opens the modal and enters a device name.
 * 2. startRegistration() records a baseline reading (power + current before the
 *    appliance is switched on), then starts a 30-second 1-Hz sampling loop.
 * 3. Each tick, the latest sensor values from lastData are pushed into nilmSamples[].
 * 4. finishRegistration() averages all samples, computes deltas vs. the baseline,
 *    and POSTs the signature to /nilm.php?action=register.
 *
 * AI ANALYTICS
 * ------------
 * fetchAnalytics() polls analytics.php every 5 minutes. On each response it
 * renders the energy rating badge, predicted bill, recommendation cards, and
 * the next-generation countdown. The backend only calls Gemini at 6 AM / 6 PM,
 * so most polls receive cached data instantly.
 *
 * NILM DETECTION
 * --------------
 * runDetection() is called from inside fetchData() but is internally throttled
 * to fire at most once every 4 seconds to avoid flooding nilm.php while still
 * keeping the "active devices" display responsive.
 */

const API_BASE  = "https://pmo.infinityfree.me";
const DEVICE_ID = "PMO-ESP32-001";

// ── Chart configuration ───────────────────────────────────────────
const chartFont = { family: "'Space Mono', monospace", size: 11 };
const gridColor = '#a8c0a0';
const tickColor = '#4d7a58';

const baseOpts = {
  animation: false,
  responsive: true,
  plugins: { legend: { display: false } },
  scales: {
    x: { ticks: { font: chartFont, color: tickColor, maxTicksLimit: 7 }, grid: { color: gridColor } },
    y: { ticks: { font: chartFont, color: tickColor }, grid: { color: gridColor } }
  }
};

function makeChart(id, datasets, extraOpts = {}) {
  const opts = JSON.parse(JSON.stringify(baseOpts));
  Object.assign(opts.scales, extraOpts.scales || {});
  if (extraOpts.plugins) Object.assign(opts.plugins, extraOpts.plugins);
  return new Chart(document.getElementById(id), { type: 'line', data: { labels: [], datasets }, options: opts });
}

// Charts are only instantiated if both Chart.js is loaded and the canvas element
// exists on the current page — pages that don't include a chart canvas stay null.
const hasChart = typeof Chart !== 'undefined';

const chartVolt = (hasChart && document.getElementById('chartVolt'))
  ? makeChart('chartVolt', [{
      label: 'Voltage (V)', data: [],
      borderColor: '#1565c0', backgroundColor: '#1565c022',
      tension: 0.35, pointRadius: 2, borderWidth: 2.5
    }]) : null;

const chartPow = (hasChart && document.getElementById('chartPow'))
  ? makeChart('chartPow', [
      { label: 'Active W',     data: [], borderColor: '#1b5e20', backgroundColor: '#1b5e2022', tension: 0.35, pointRadius: 1.5, borderWidth: 2.5 },
      { label: 'Apparent VA',  data: [], borderColor: '#e65100', backgroundColor: '#e6510022', tension: 0.35, pointRadius: 1.5, borderWidth: 2.5 },
      { label: 'Reactive VAR', data: [], borderColor: '#6a1b9a', backgroundColor: '#6a1b9a22', tension: 0.35, pointRadius: 1.5, borderWidth: 2.5 }
    ]) : null;

// chartCurrPf uses a dual Y-axis: current on the left (y), power factor on the
// right (y1). The right axis has drawOnChartArea:false so its grid lines don't
// overdraw the current grid lines.
const chartCurrPf = (hasChart && document.getElementById('chartCurrPf'))
  ? makeChart('chartCurrPf', [
      { label: 'Current A',    data: [], borderColor: '#b71c1c', backgroundColor: '#b71c1c22', tension: 0.35, pointRadius: 1.5, borderWidth: 2.5, yAxisID: 'y'  },
      { label: 'Power Factor', data: [], borderColor: '#0277bd', backgroundColor: '#0277bd22', tension: 0.35, pointRadius: 1.5, borderWidth: 2.5, yAxisID: 'y1' }
    ], {
      scales: {
        y1: {
          type: 'linear', position: 'right',
          ticks: { font: chartFont, color: '#0277bd' },
          grid:  { drawOnChartArea: false, color: gridColor }
        }
      }
    }) : null;

const chartFreq = (hasChart && document.getElementById('chartFreq'))
  ? makeChart('chartFreq', [{
      label: 'Frequency Hz', data: [],
      borderColor: '#4a148c', backgroundColor: '#4a148c22',
      tension: 0.35, pointRadius: 2, borderWidth: 2.5
    }]) : null;

// ── State ────────────────────────────────────────────────────────
let lastRowId  = null;
let staleCount = 0;
let isStale    = false;
let wasStale   = false;
let lastData   = null;

// ── Stale detection ──────────────────────────────────────────────

// setStale() is called after 5 consecutive polls return the same row id,
// indicating the ESP32 has stopped uploading (offline, crashed, or no WiFi).
function setStale() {
  if (isStale) return;
  isStale  = true;
  wasStale = true;
  const connStatus = document.getElementById('connStatus');
  const connDot    = document.getElementById('connDot');
  const lastUpdate = document.getElementById('lastUpdate');
  if (connStatus) connStatus.textContent = 'Stale';
  if (connDot)    connDot.className      = 'offline-dot';
  if (lastUpdate) lastUpdate.style.color = 'var(--red)';
  document.querySelectorAll('.metric-card').forEach(c => c.classList.add('stale'));
  // Disable relay buttons while stale — commands sent now would queue in the
  // database and execute unexpectedly when the device reconnects.
  document.querySelectorAll('.relay-buttons .btn').forEach(b => {
    b.disabled = true; b.style.opacity = '0.4'; b.style.cursor = 'not-allowed';
  });
}

function clearStale() {
  isStale    = false;
  staleCount = 0;
  const lastUpdate = document.getElementById('lastUpdate');
  if (lastUpdate) lastUpdate.style.color = '';
  document.querySelectorAll('.metric-card').forEach(c => c.classList.remove('stale'));
  document.querySelectorAll('.relay-buttons .btn').forEach(b => {
    b.disabled = false; b.style.opacity = ''; b.style.cursor = '';
  });
  if (wasStale) {
    wasStale = false;
    const connStatus = document.getElementById('connStatus');
    const connDot    = document.getElementById('connDot');
    if (connStatus) connStatus.textContent = 'Reconnected';
    if (connDot)    connDot.className      = 'live-dot';
    setTimeout(() => {
      const el = document.getElementById('connStatus');
      if (el) el.textContent = 'Live';
    }, 2000);
  }
}

// Compare the latest row id from data.php against the previous poll's id.
// Incrementing staleCount only when the id hasn't changed means brief server
// latency spikes (one or two identical responses) don't trigger a stale state.
function checkStaleness(currentId) {
  if (lastRowId === null) { lastRowId = currentId; return; }
  if (currentId === lastRowId) {
    staleCount++;
    if (staleCount >= 5) setStale();
  } else {
    lastRowId = currentId;
    clearStale();
  }
}

// ── Helpers ──────────────────────────────────────────────────────
function fmtTime(ts) {
  if (!ts) return '---';
  return new Date(ts).toLocaleTimeString('en-PH', { hour12: false });
}
function fmtNum(v, dec = 2) {
  if (v === null || v === undefined || isNaN(parseFloat(v))) return '---';
  return parseFloat(v).toFixed(dec);
}

// setCardState applies a CSS class (ok / warn / danger) to a metric card based
// on symmetric threshold bands. dangerLow/dangerHigh are the outer thresholds
// (relay trip zones), warnLow/warnHigh are the inner advisory bands.
function setCardState(id, val, warnLow, dangerLow, warnHigh, dangerHigh) {
  const el = document.getElementById(id);
  if (!el) return;
  el.classList.remove('ok', 'warn', 'danger');
  if      (val <= dangerLow || val >= dangerHigh) el.classList.add('danger');
  else if (val <= warnLow   || val >= warnHigh)   el.classList.add('warn');
  else                                             el.classList.add('ok');
}

// seedChart initialises all datasets at once from the 50-row history snapshot.
// Called only once (historySeeded guards it) to avoid wiping in-flight pushPoint data.
function seedChart(chart, rows, ...extractors) {
  if (!chart) return;
  chart.data.labels = rows.map(r => fmtTime(r.recorded_at));
  chart.data.datasets.forEach((ds, i) => { ds.data = rows.map(r => parseFloat(extractors[i](r)) || 0); });
  chart.update('none');
}

// pushPoint appends one new data point and enforces the 60-point sliding window
// by shifting the oldest label and dataset values off the front of the arrays.
function pushPoint(chart, label, ...values) {
  if (!chart) return;
  chart.data.labels.push(label);
  values.forEach((v, i) => chart.data.datasets[i].data.push(v));
  if (chart.data.labels.length > 60) {
    chart.data.labels.shift();
    chart.data.datasets.forEach(ds => ds.data.shift());
  }
  chart.update('none');
}

// ── Card updates ─────────────────────────────────────────────────
let lastSeenId    = null;
let historySeeded = false;

// animateValue triggers a CSS flash animation on the element whenever the
// displayed value changes, giving a visual pulse that shows data is live.
function animateValue(elId, newVal) {
  const el = document.getElementById(elId);
  if (!el || el.textContent === newVal) return;
  el.textContent = newVal;
  el.classList.remove('flash');
  void el.offsetWidth; // force reflow so the animation restarts from scratch
  el.classList.add('flash');
  setTimeout(() => el.classList.remove('flash'), 400);
}

function updateCards(L) {
  const wifiSsid   = document.getElementById('wifiSsid');
  const connStatus = document.getElementById('connStatus');
  const lastUpdate = document.getElementById('lastUpdate');
  const connDot    = document.getElementById('connDot');

  if (wifiSsid)   wifiSsid.textContent   = L.wifi_ssid || '---';
  if (connStatus) connStatus.textContent = 'Live';
  if (lastUpdate) lastUpdate.textContent = 'Updated: ' + fmtTime(L.recorded_at);
  if (connDot)    connDot.className      = 'live-dot';

  // Relay status indicator — shows ON/OFF state and the trip reason badge.
  const relEl  = document.getElementById('relayStatus');
  const iconEl = document.getElementById('relayIcon');
  const tripEl = document.getElementById('tripMsg');
  if (relEl && iconEl) {
    const relOn = L.relay_state == 1;
    relEl.textContent  = relOn ? 'ON' : 'OFF';
    relEl.className    = 'relay-text-value ' + (relOn ? 'on' : 'off');
    iconEl.className   = 'relay-icon ' + (relOn ? 'on' : 'off');
    iconEl.textContent = relOn ? '⚡' : '○';
  }
  if (tripEl) {
    if (L.trip_reason && L.trip_reason !== 'None') {
      tripEl.textContent = 'Trip: ' + L.trip_reason;
      tripEl.style.display = 'inline-block';
    } else {
      tripEl.style.display = 'none';
    }
  }

  // Animate every metric value individually so only changed fields flash.
  animateValue('mVolt',   fmtNum(L.voltage, 1));
  animateValue('mCurr',   fmtNum(L.current_a, 3));
  animateValue('mPow',    fmtNum(L.power_w, 1));
  animateValue('mApp',    fmtNum(L.apparent_power, 1));
  animateValue('mReact',  fmtNum(L.reactive_power, 1));
  animateValue('mFreq',   fmtNum(L.frequency_hz, 1));
  animateValue('mPf',     fmtNum(L.power_factor, 3));
  animateValue('mEnergy', fmtNum(L.energy_kwh, 4));
  animateValue('mCostH',  fmtNum(L.cost_per_hour, 2));
  animateValue('mCostM',  fmtNum(L.cost_per_month, 2));
  animateValue('mWaste',  fmtNum(L.wasted_power, 1));
  animateValue('mMargin', fmtNum(L.safety_margin, 1));
  animateValue('mCo2',    fmtNum(L.co2_kg, 4));
  const mLoad = document.getElementById('mLoad');
  if (mLoad) mLoad.textContent = L.load_type || '---';

  // Threshold bands mirror the relay protection limits defined in the firmware
  // so the web cards turn amber / red at the same values the hardware trips on.
  setCardState('cVolt',   parseFloat(L.voltage),       210, 200, 230, 240);
  setCardState('cFreq',   parseFloat(L.frequency_hz),  59,  58,  61,  62);
  setCardState('cMargin', parseFloat(L.safety_margin), 30,  20,  101, 101);
  setCardState('cWaste',  parseFloat(L.wasted_power),  0,   0,   200, 400);
}

function rebuildTable(rows) {
  const tbody = document.getElementById('histBody');
  if (!tbody) return;
  tbody.innerHTML = '';
  [...rows].reverse().slice(0, 30).forEach(row => {
    const tr = document.createElement('tr');
    tr.innerHTML = `
      <td>${fmtTime(row.recorded_at)}</td>
      <td>${fmtNum(row.voltage,1)}</td>
      <td>${fmtNum(row.current_a,3)}</td>
      <td>${fmtNum(row.power_w,1)}</td>
      <td>${fmtNum(row.apparent_power,1)}</td>
      <td>${fmtNum(row.reactive_power,1)}</td>
      <td>${fmtNum(row.frequency_hz,1)}</td>
      <td>${fmtNum(row.power_factor,3)}</td>
      <td>${fmtNum(row.energy_kwh,4)}</td>`;
    tbody.appendChild(tr);
  });
}

// ── Data fetch ───────────────────────────────────────────────────

// fetchData is the main 1-second polling loop. A cache-busting ?t= query
// parameter prevents the browser or CDN from returning stale responses.
async function fetchData() {
  try {
    const res  = await fetch(`${API_BASE}/data.php?t=${Date.now()}`);
    const json = await res.json();
    const L = json.latest, H = json.history;
    if (!L) return;

    lastData = L;
    updateCards(L);
    checkStaleness(parseInt(L.id));

    if (H && H.length > 0) {
      if (!historySeeded) {
        // First fetch: seed all charts from the 50-row history snapshot.
        seedChart(chartVolt,   H, r => r.voltage);
        seedChart(chartPow,    H, r => r.power_w, r => r.apparent_power || 0, r => r.reactive_power || 0);
        seedChart(chartCurrPf, H, r => r.current_a, r => r.power_factor);
        seedChart(chartFreq,   H, r => r.frequency_hz);
        rebuildTable(H);
        lastSeenId    = parseInt(L.id);
        historySeeded = true;
      }
      // Subsequent fetches: only push if a new row has arrived (id changed).
      const latestId = parseInt(L.id);
      if (latestId !== lastSeenId) {
        const label = fmtTime(L.recorded_at);
        pushPoint(chartVolt,   label, parseFloat(L.voltage));
        pushPoint(chartPow,    label, parseFloat(L.power_w), parseFloat(L.apparent_power || 0), parseFloat(L.reactive_power || 0));
        pushPoint(chartCurrPf, label, parseFloat(L.current_a), parseFloat(L.power_factor));
        pushPoint(chartFreq,   label, parseFloat(L.frequency_hz));
        rebuildTable(H);
        lastSeenId = latestId;
      }
    }
    runDetection();
  } catch(e) {
    const connStatus = document.getElementById('connStatus');
    const connDot    = document.getElementById('connDot');
    if (connStatus) connStatus.textContent = 'Offline';
    if (connDot)    connDot.className      = 'offline-dot';
    staleCount++;
    if (staleCount >= 5) setStale();
  }
}

async function sendRelay(cmd) {
  try {
    await fetch(`${API_BASE}/relay.php`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ device_id: DEVICE_ID, command: cmd })
    });
  } catch(e) { console.error('Relay command failed', e); }
}

fetchData();
setInterval(fetchData, 1000);

// ── Accordion ────────────────────────────────────────────────────
function toggleAccordion() {
  const body = document.getElementById('histAccordionBody');
  const btn  = document.getElementById('histAccordionBtn');
  if (!body || !btn) return;
  body.classList.toggle('open');
  btn.classList.toggle('open');
}

// ── Modal ────────────────────────────────────────────────────────
function openModal() {
  const modal = document.getElementById('nilmModal');
  if (!modal) return;
  modal.classList.add('open');
  document.body.style.overflow = 'hidden';
  setTimeout(() => {
    const inp = document.getElementById('nilmDeviceName');
    if (inp) inp.focus();
  }, 300);
}

function closeModal() {
  if (nilmRegistering) {
    clearInterval(nilmInterval);
    nilmRegistering = false;
    const prog = document.getElementById('nilmProgress');
    const stat = document.getElementById('nilmRegStatus');
    if (prog) prog.style.display = 'none';
    if (stat) stat.textContent  = '';
  }
  const modal = document.getElementById('nilmModal');
  if (modal) modal.classList.remove('open');
  document.body.style.overflow = '';
}

function closeModalOutside(e) {
  if (e.target === document.getElementById('nilmModal')) closeModal();
}

// ── NILM Registration ─────────────────────────────────────────────
// These state variables are shared between startRegistration() and
// finishRegistration() across the 30-second sampling window.
let nilmSamples     = [];
let nilmRegistering = false;
let nilmBaseline    = null;   // readings captured before the appliance is turned on
let nilmInterval    = null;

function startRegistration() {
  const nameEl = document.getElementById('nilmDeviceName');
  const name   = nameEl ? nameEl.value.trim() : '';
  if (!name) { alert('Enter a device name first.'); return; }
  if (nilmRegistering) return;

  nilmRegistering = true;
  nilmSamples     = [];
  const regStatus = document.getElementById('nilmRegStatus');
  const progress  = document.getElementById('nilmProgress');
  if (regStatus) regStatus.textContent  = '';
  if (progress)  progress.style.display = 'block';

  // Animate the progress bar draining over 30 seconds using a CSS transition.
  // transition is reset to 'none' first to ensure it restarts from 100%.
  const fill = document.getElementById('nilmProgressFill');
  if (fill) {
    fill.style.transition = 'none';
    fill.style.width = '100%';
    void fill.offsetWidth;
    fill.style.transition = 'width 30s linear';
    fill.style.width = '0%';
  }

  // Record baseline (circuit load before the target appliance is switched on).
  // The delta fields in the registered signature = averaged sample − baseline,
  // representing the appliance's incremental contribution to the circuit.
  nilmBaseline = {
    power:   parseFloat((lastData && lastData.power_w)   || 0),
    current: parseFloat((lastData && lastData.current_a) || 0),
  };

  let t = 30;
  const countdown = document.getElementById('nilmCountdown');
  nilmInterval = setInterval(() => {
    if (countdown) countdown.textContent = --t; else t--;
    // Capture the current sensor snapshot into the sample buffer.
    nilmSamples.push({
      power:    parseFloat((lastData && lastData.power_w)        || 0),
      current:  parseFloat((lastData && lastData.current_a)      || 0),
      pf:       parseFloat((lastData && lastData.power_factor)   || 0),
      apparent: parseFloat((lastData && lastData.apparent_power) || 0),
    });
    if (t <= 0) {
      clearInterval(nilmInterval);
      finishRegistration(name);
    }
  }, 1000);
}

async function finishRegistration(name) {
  const progress = document.getElementById('nilmProgress');
  if (progress) progress.style.display = 'none';
  nilmRegistering = false;

  // Average all 30 collected samples to get a stable device fingerprint.
  const avg = (key) => nilmSamples.reduce((s, r) => s + r[key], 0) / nilmSamples.length;

  const payload = {
    device_name:   name,
    avg_power:     avg('power'),
    avg_current:   avg('current'),
    avg_pf:        avg('pf'),
    avg_apparent:  avg('apparent'),
    delta_power:   avg('power')   - ((nilmBaseline && nilmBaseline.power)   || 0),
    delta_current: avg('current') - ((nilmBaseline && nilmBaseline.current) || 0),
  };

  await fetch(`${API_BASE}/nilm.php?action=register`, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(payload)
  });

  const regStatus = document.getElementById('nilmRegStatus');
  const nameEl    = document.getElementById('nilmDeviceName');
  if (regStatus) regStatus.textContent = `✓ "${name}" registered!`;
  if (nameEl)    nameEl.value = '';
  loadSignatures();

  setTimeout(() => {
    closeModal();
    const s = document.getElementById('nilmRegStatus');
    if (s) s.textContent = '';
  }, 2000);
}

// ── AI Analytics ─────────────────────────────────────────────────

// Analytics are polled every 5 minutes. The backend only calls Gemini at
// 6 AM / 6 PM so most polls return cached data without any LLM latency.
async function fetchAnalytics() {
  if (!document.getElementById('aiRating')) return;
  try {
    const res  = await fetch(`${API_BASE}/analytics.php`);
    const data = await res.json();

    if (data.latest) updateAnalyticsDisplay(data.latest);

    if (data.next_at) {
      const next   = new Date(data.next_at);
      const nextEl = document.getElementById('aiNextUpdate');
      if (nextEl) nextEl.textContent =
        'Next: ' + next.toLocaleDateString('en-PH', { month: 'short', day: 'numeric' }) +
        ' '      + next.toLocaleTimeString('en-PH', { hour12: true, hour: 'numeric', minute: '2-digit' });
    }
  } catch(e) { console.error('Analytics fetch failed', e); }
}

function updateAnalyticsDisplay(rec) {
  const ratingEl = document.getElementById('aiRating');
  if (!ratingEl) return;
  const rating   = (rec.energy_rating || '--').toUpperCase();
  ratingEl.textContent = rating;
  ratingEl.className   = 'rating-badge rating-' + getRatingClass(rating);

  const scoreEl = document.getElementById('aiScore');
  if (scoreEl) scoreEl.textContent =
    'Score: ' + (rec.rating_score != null ? rec.rating_score + ' / 100' : '---');

  const billEl = document.getElementById('aiBill');
  if (billEl) {
    const bill = parseFloat(rec.predicted_bill || 0);
    billEl.textContent = bill
      ? '₱' + bill.toLocaleString('en-PH', { minimumFractionDigits: 2, maximumFractionDigits: 2 })
      : '₱---';
  }

  const summaryEl = document.getElementById('aiSummary');
  if (summaryEl) summaryEl.textContent = rec.summary || '---';

  // recommendations is stored as a JSON string in the database.
  let recs = [];
  try { recs = JSON.parse(rec.recommendations || '[]'); } catch(e) {}
  const recsEl = document.getElementById('aiRecs');
  if (recsEl) recsEl.innerHTML = recs.length
    ? recs.map((r, i) => `
        <div class="ai-rec-card">
          <div class="ai-rec-num">${String(i + 1).padStart(2, '0')}</div>
          <div class="ai-rec-text">${r}</div>
        </div>`).join('')
    : '<div class="ai-loading">No recommendations in this analysis.</div>';

  const tipEl = document.getElementById('aiTip');
  if (tipEl) tipEl.textContent = rec.efficiency_tip || '---';

  const genDate    = new Date(rec.generated_at);
  const periodIcon = rec.period === 'morning' ? '☀' : '🌙';
  const lastUpdEl  = document.getElementById('aiLastUpdated');
  if (lastUpdEl) lastUpdEl.textContent =
    periodIcon + ' ' + genDate.toLocaleString('en-PH', {
      month: 'short', day: 'numeric', hour: 'numeric', minute: '2-digit', hour12: true
    });
}

// Map Gemini's letter grade to a CSS class for the coloured rating badge.
function getRatingClass(rating) {
  const r   = (rating || '').charAt(0).toUpperCase();
  const map = { A: 'a', B: 'b', C: 'c', D: 'd', F: 'f' };
  return map[r] || 'default';
}

fetchAnalytics();
setInterval(fetchAnalytics, 5 * 60 * 1000);

// ── Signatures ───────────────────────────────────────────────────
async function loadSignatures() {
  if (!document.getElementById('sigCount') && !document.getElementById('sigsBody')) return;
  try {
    const res  = await fetch(`${API_BASE}/nilm.php?action=list`);
    const sigs = await res.json();

    const countEl = document.getElementById('sigCount');
    if (countEl) countEl.textContent = sigs.length;

    const tbody = document.getElementById('sigsBody');
    if (!tbody) return;
    if (!sigs.length) {
      tbody.innerHTML = '<tr><td colspan="6" style="text-align:center;color:var(--text3);padding:20px;">No signatures yet.</td></tr>';
      return;
    }
    tbody.innerHTML = sigs.map(s => `
      <tr>
        <td>${s.device_name}</td>
        <td>${parseFloat(s.avg_power).toFixed(1)}</td>
        <td>${parseFloat(s.avg_current).toFixed(3)}</td>
        <td>${parseFloat(s.avg_pf).toFixed(3)}</td>
        <td>${parseFloat(s.avg_apparent).toFixed(1)}</td>
        <td>${new Date(s.created_at).toLocaleString('en-PH')}</td>
      </tr>`).join('');
  } catch(e) { console.error('loadSignatures failed', e); }
}

loadSignatures();

// ── NILM Detection ───────────────────────────────────────────────
let lastDetectionTime = 0;

// runDetection posts the current live readings to nilm.php for server-side
// subset-search inference. Throttled to 4-second intervals to avoid hammering
// the backend on every 1-second fetchData tick.
async function runDetection() {
  if (isStale || !lastData) return;
  const now = Date.now();
  if (now - lastDetectionTime < 4000) return;
  lastDetectionTime = now;

  const pw   = parseFloat(lastData.power_w);
  const curr = parseFloat(lastData.current_a);
  const pf   = parseFloat(lastData.power_factor);
  if (isNaN(pw) || isNaN(curr)) return;

  try {
    const res  = await fetch(`${API_BASE}/nilm.php?action=detect`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ power: pw, current: curr, pf })
    });
    const data   = await res.json();
    const listEl = document.getElementById('nilmMatchList');
    const confEl = document.getElementById('nilmConf');
    if (!listEl || !confEl) return;

    if (data.matches && data.matches.length > 0) {
      // Render one chip per matched appliance showing its name and registered wattage.
      listEl.innerHTML = data.matches.map(m => `
        <div class="nilm-chip">
          ${m.device}
          <span class="chip-watts">${parseFloat(m.avg_power).toFixed(1)}W</span>
        </div>`).join('');
      confEl.textContent = data.confidence + '%';
    } else {
      listEl.innerHTML = '<span style="font-family:\'Space Mono\',monospace; font-size:14px; color:var(--text3);">No match</span>';
      confEl.textContent = '---';
    }
  } catch(e) { console.error('Detection failed', e); }
}

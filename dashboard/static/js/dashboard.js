/* ================================================================
   EmbryoGuard Pro — Dashboard JavaScript v1.5b
   WebSocket real-time + egg selector + AUTO/MANUAL lock
   ================================================================ */

const socket = io();

// ── EGG PROFILE DATA (mirrors firmware) ─────────────────────────
const EGG_PROFILES = {
  chicken: { name: 'Chicken', emoji: '🐔', temp_target: 37.5, temp_cold: 36.5, temp_hot: 38.5, days: 21 },
  duck:    { name: 'Duck',    emoji: '🦆', temp_target: 37.7, temp_cold: 37.0, temp_hot: 39.0, days: 28 },
  quail:   { name: 'Quail',   emoji: '🐦', temp_target: 37.8, temp_cold: 37.5, temp_hot: 39.5, days: 18 },
  goose:   { name: 'Goose',   emoji: '🦢', temp_target: 37.6, temp_cold: 36.8, temp_hot: 38.8, days: 25 },
};

// ── GLOBAL STATE ────────────────────────────────────────────────
let tChart = null, hChart = null;
let currentMode = 'AUTO';
let currentEgg = 'chicken';
let lastManualCommandTs = 0;           // timestamp of last manual fan/lamp/servo command
const MANUAL_GRACE_PERIOD_MS = 6000;   // don't let sensor_update overwrite buttons for 6s after manual cmd

// ── INITIALIZATION ──────────────────────────────────────────────
document.addEventListener('DOMContentLoaded', () => {
  initCharts();
  fetchServerInfo();
  fetchInitialState();
  setInterval(updateClock, 1000);
  updateClock();
  applyModeLock('AUTO');  // default AUTO lock on load
});

function fetchServerInfo() {
  fetch('/api/server-info')
    .then(res => res.json())
    .then(data => {
      const el = document.getElementById('server-ip');
      const qr = document.getElementById('qr-code-container');
      if (el) el.innerText = data.url;
      if (qr && window.QRCode) {
        new QRCode(qr, {
          text: data.url, width: 120, height: 120,
          colorDark: '#2E1503', colorLight: '#ffffff',
          correctLevel: QRCode.CorrectLevel.H
        });
      }
    }).catch(() => {});
}

function fetchInitialState() {
  fetch('/api/state')
    .then(res => res.json())
    .then(data => {
      if (data.settings) {
        currentEgg = data.settings.egg_type || 'chicken';
        updateEggUI(currentEgg);
      }
      if (data.nodes && data.nodes['1']) {
        currentMode = data.nodes['1'].mode || 'AUTO';
        applyModeLock(currentMode);
      }
    }).catch(() => {});
}

// ── WEBSOCKET LISTENERS ─────────────────────────────────────────
socket.on('sensor_update', data => {
  updateNode1(data.nodes['1']);
  updateNode2(data.nodes['2']);
  updateAlerts(data.alerts);
  updateCharts(data.history);
});

socket.on('egg_type_update', data => {
  currentEgg = data.egg_type;
  updateEggUI(data.egg_type);
});

socket.on('command_ack', data => {
  if (data.command === 'mode') {
    currentMode = data.value;
    applyModeLock(data.value);
  }
});

// ── EGG SELECTION ───────────────────────────────────────────────
function selectEgg(eggType) {
  // Optimistic UI
  updateEggUI(eggType);
  
  fetch('/api/egg-type', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ egg_type: eggType })
  })
  .then(res => res.json())
  .then(data => {
    if (data.status === 'ok') {
      currentEgg = data.egg_type;
      updateEggUI(data.egg_type);
    }
  })
  .catch(err => console.error('Egg type error:', err));
}

function updateEggUI(eggType) {
  // Highlight active button
  document.querySelectorAll('.egg-btn').forEach(btn => btn.classList.remove('active'));
  const btn = document.getElementById('egg-' + eggType);
  if (btn) btn.classList.add('active');
  
  // Update target display
  const profile = EGG_PROFILES[eggType];
  const display = document.getElementById('egg-target-display');
  if (display && profile) {
    display.innerText = profile.temp_target + '°C';
  }
}

// ── MODE CONTROL (AUTO / MANUAL) ────────────────────────────────
function setMode(mode) {
  sendControl(1, 'mode', mode);
  currentMode = mode;
  applyModeLock(mode);
}

function applyModeLock(mode) {
  const isAuto = (mode === 'AUTO');
  
  // Toggle active state on mode buttons
  const autoBtn = document.getElementById('mode-auto-btn');
  const manBtn = document.getElementById('mode-manual-btn');
  if (autoBtn) autoBtn.classList.toggle('active', isAuto);
  if (manBtn) manBtn.classList.toggle('active', !isAuto);
  
  // Lock/unlock manual control rows
  document.querySelectorAll('.manual-control').forEach(row => {
    const buttons = row.querySelectorAll('button');
    buttons.forEach(btn => {
      btn.disabled = isAuto;
      btn.classList.toggle('locked', isAuto);
    });
    row.classList.toggle('row-locked', isAuto);
  });
  
  // Show/hide lock notice
  const notice = document.getElementById('auto-lock-notice');
  if (notice) notice.classList.toggle('visible', isAuto);
  
  // Update mode status text
  const modeStatus = document.getElementById('mode-status');
  if (modeStatus) modeStatus.innerText = mode;
}

// ── SEND CONTROL COMMAND ────────────────────────────────────────
function sendControl(node, command, value) {
  socket.emit('control', { node: node, command: command, value: value });
  
  // ── Optimistic UI: update buttons INSTANTLY on click ──────────
  if (node === 1 && (command === 'fan' || command === 'lamp' || command === 'servo')) {
    lastManualCommandTs = Date.now();
    
    if (command === 'fan') {
      const fanOn = document.getElementById('fan-on-btn');
      const fanOff = document.getElementById('fan-off-btn');
      const fanStatus = document.getElementById('fan-status');
      if (fanOn) fanOn.classList.toggle('active', !!value);
      if (fanOff) fanOff.classList.toggle('active', !value);
      if (fanStatus) fanStatus.innerText = value ? 'ON' : 'OFF';
    }
    else if (command === 'lamp') {
      const isOn = (value === 'FULL');
      const lampOn = document.getElementById('lamp-on-btn');
      const lampOff = document.getElementById('lamp-off-btn');
      const lampStatus = document.getElementById('lamp-status');
      if (lampOn) lampOn.classList.toggle('active', isOn);
      if (lampOff) lampOff.classList.toggle('active', !isOn);
      if (lampStatus) lampStatus.innerText = isOn ? 'ON' : 'OFF';
    }
    else if (command === 'servo') {
      const servoStatus = document.getElementById('servo-status');
      if (servoStatus) servoStatus.innerText = 'Turning...';
    }
  }
}

// ── CLOCK ───────────────────────────────────────────────────────
function updateClock() {
  const el = document.getElementById('clock');
  if (el) el.innerText = new Date().toLocaleTimeString();
}

// ── ALERT STATUS ────────────────────────────────────────────────
function updateAlerts(alerts) {
  const el = document.getElementById('sys-status');
  if (!el) return;
  if (!alerts || alerts.length === 0) {
    el.innerHTML = '<span class="status-dot ok"></span><span class="status-text">All Systems Normal</span>';
  } else {
    const level = alerts.some(a => a.level === 'danger') ? 'danger' : 'warning';
    el.innerHTML = `<span class="status-dot ${level}"></span><span class="status-text">${alerts.length} Alert(s)</span>`;
  }
}

// ── NODE 1 UPDATE ───────────────────────────────────────────────
function updateNode1(n1) {
  if (!n1) return;
  
  // Online status
  const statusEl = document.getElementById('n1-status');
  if (statusEl) {
    statusEl.innerHTML = n1.online
      ? '<span class="status-dot ok"></span><span>Online</span>'
      : '<span class="status-dot offline"></span><span>Offline</span>';
  }
  
  // Gauges
  setGauge('n1-temp', n1.temperature, 20, 50);
  setGauge('n1-hum', n1.humidity, 0, 100);
  
  // Gauge badges — color code them
  const tBadge = document.getElementById('n1-temp-badge');
  const hBadge = document.getElementById('n1-hum-badge');
  if (tBadge) {
    if (!n1.sensor_ok) {
      tBadge.innerText = 'SENSOR ERR';
      tBadge.className = 'gauge-badge danger';
    } else if (n1.temperature === null) {
      tBadge.innerText = 'N/A';
      tBadge.className = 'gauge-badge';
    } else if (n1.temperature >= 39.0 || n1.temperature <= 35.0) {
      tBadge.innerText = 'DANGER';
      tBadge.className = 'gauge-badge danger';
    } else if (n1.temperature >= 38.5 || n1.temperature <= 36.5) {
      tBadge.innerText = 'WARNING';
      tBadge.className = 'gauge-badge warning';
    } else {
      tBadge.innerText = 'OPTIMAL';
      tBadge.className = 'gauge-badge ok';
    }
  }
  if (hBadge) {
    if (n1.humidity === null) {
      hBadge.innerText = 'N/A';
      hBadge.className = 'gauge-badge';
    } else if (n1.humidity >= 75 || n1.humidity <= 38) {
      hBadge.innerText = 'DANGER';
      hBadge.className = 'gauge-badge danger';
    } else if (n1.humidity >= 65 || n1.humidity <= 45) {
      hBadge.innerText = 'WARNING';
      hBadge.className = 'gauge-badge warning';
    } else {
      hBadge.innerText = 'OPTIMAL';
      hBadge.className = 'gauge-badge ok';
    }
  }
  
  // Incubation progress
  const profile = EGG_PROFILES[currentEgg] || EGG_PROFILES['chicken'];
  const maxDay = profile.days;
  const hatchDay = maxDay - 3; // Last 3 days are hatching phase
  const day = Math.min(n1.incubation_day || 1, maxDay);

  const dayEl = document.getElementById('inc-day');
  const progEl = document.getElementById('inc-progress');
  const phaseEl = document.getElementById('inc-phase');
  if (dayEl) dayEl.innerText = `Day ${day} / ${maxDay}`;
  if (progEl) progEl.style.width = `${(day / maxDay) * 100}%`;
  if (phaseEl) phaseEl.innerText = day > hatchDay ? 'HATCHING PHASE' : 'INCUBATION PHASE';
  
  // Mode
  const isAuto = (n1.mode === 'AUTO');
  if (n1.mode !== currentMode) {
    currentMode = n1.mode;
    applyModeLock(n1.mode);
  }
  
  // Egg type from firmware
  if (n1.egg_type && n1.egg_type !== currentEgg) {
    currentEgg = n1.egg_type;
    updateEggUI(n1.egg_type);
  }
  
  // Actuator statuses — skip overwriting if we recently sent a manual command
  const inGracePeriod = (currentMode === 'MANUAL') && (Date.now() - lastManualCommandTs < MANUAL_GRACE_PERIOD_MS);
  
  const modeStatus = document.getElementById('mode-status');
  const servoStatus = document.getElementById('servo-status');
  
  if (modeStatus) modeStatus.innerText = n1.mode || 'AUTO';
  if (servoStatus) servoStatus.innerText = `${n1.servo_angle || 0}° (×${n1.servo_turns || 0})`;
  
  if (!inGracePeriod) {
    // Only update fan/lamp from server data when NOT in manual grace period
    const fanStatus = document.getElementById('fan-status');
    const lampStatus = document.getElementById('lamp-status');
    if (fanStatus) fanStatus.innerText = n1.fan ? 'ON' : 'OFF';
    if (lampStatus) lampStatus.innerText = n1.lamp === 'FULL' ? 'ON' : 'OFF';
    
    // Active button highlights (fan)
    const fanOn = document.getElementById('fan-on-btn');
    const fanOff = document.getElementById('fan-off-btn');
    if (fanOn) fanOn.classList.toggle('active', !!n1.fan);
    if (fanOff) fanOff.classList.toggle('active', !n1.fan);
    
    // Active button highlights (lamp)
    const lampOn = document.getElementById('lamp-on-btn');
    const lampOff = document.getElementById('lamp-off-btn');
    if (lampOn) lampOn.classList.toggle('active', n1.lamp === 'FULL');
    if (lampOff) lampOff.classList.toggle('active', n1.lamp !== 'FULL');
  }
}

// ── NODE 2 UPDATE ───────────────────────────────────────────────
function updateNode2(n2) {
  if (!n2) return;
  
  const statusEl = document.getElementById('n2-status');
  if (statusEl) {
    statusEl.innerHTML = n2.online
      ? '<span class="status-dot ok"></span><span>Online</span>'
      : '<span class="status-dot offline"></span><span>Offline</span>';
  }
  
  setText('n2-temp-val', n2.temperature !== null && n2.temperature !== undefined ? `${n2.temperature.toFixed(1)} °C` : '-- °C');
  setText('n2-hum-val', n2.humidity !== null && n2.humidity !== undefined ? `${n2.humidity.toFixed(1)} %` : '-- %');
  setText('n2-mq-val', `ADC: ${n2.mq135_raw || 0}`);
  setText('n2-light-val', `${n2.light_pct || 0} %`);
  
  // Bars
  setWidth('n2-mq-bar', Math.min(((n2.mq135_raw || 0) / 4095) * 100, 100));
  setWidth('n2-light-bar', n2.light_pct || 0);
  
  // Badges
  setBadge('n2-temp-badge', n2.temperature, 35, 36.5, 38.5, 39);
  setBadge('n2-hum-badge', n2.humidity, 38, 45, 65, 75);
  setMqBadge('n2-mq-badge', n2.mq135_raw || 0, n2.mq135_dout);
  setLightBadge('n2-light-badge', n2.light_pct || 0);
  
  // Footer
  setText('n2-wifi', n2.wifi_ok ? 'WiFi: OK' : 'WiFi: --');
  setText('n2-cycle', `Cycle: ${n2.cycle || 0}`);
}

// ── GAUGE HELPER ────────────────────────────────────────────────
function setGauge(prefix, value, min, max) {
  const valEl = document.getElementById(`${prefix}-val`);
  const arcEl = document.getElementById(`${prefix}-arc`);
  
  if (value === null || value === undefined) {
    if (valEl) valEl.textContent = '--';
    if (arcEl) arcEl.style.strokeDashoffset = '314.16';
    return;
  }
  
  if (valEl) valEl.textContent = value.toFixed(1);
  if (arcEl) {
    const pct = Math.max(0, Math.min(1, (value - min) / (max - min)));
    arcEl.style.strokeDashoffset = (314.16 * (1 - pct)).toFixed(2);
  }
}

// ── DOM HELPERS ─────────────────────────────────────────────────
function setText(id, text) {
  const el = document.getElementById(id);
  if (el) el.innerText = text;
}

function setWidth(id, pct) {
  const el = document.getElementById(id);
  if (el) el.style.width = `${pct}%`;
}

function setBadge(id, value, dangerLow, warnLow, warnHigh, dangerHigh) {
  const el = document.getElementById(id);
  if (!el) return;
  if (value === null || value === undefined) { el.innerText = 'N/A'; el.className = 'sensor-badge'; return; }
  if (value >= dangerHigh || value <= dangerLow) { el.innerText = 'DANGER'; el.className = 'sensor-badge danger'; }
  else if (value >= warnHigh || value <= warnLow) { el.innerText = 'WARNING'; el.className = 'sensor-badge warning'; }
  else { el.innerText = 'OK'; el.className = 'sensor-badge ok'; }
}

function setMqBadge(id, raw, dout) {
  const el = document.getElementById(id);
  if (!el) return;
  if (dout === 0 || raw >= 1200) { el.innerText = 'DANGER'; el.className = 'sensor-badge danger'; }
  else if (raw >= 800) { el.innerText = 'WARNING'; el.className = 'sensor-badge warning'; }
  else { el.innerText = 'CLEAR'; el.className = 'sensor-badge ok'; }
}

function setLightBadge(id, pct) {
  const el = document.getElementById(id);
  if (!el) return;
  if (pct >= 85) { el.innerText = 'DANGER'; el.className = 'sensor-badge danger'; }
  else if (pct >= 60) { el.innerText = 'WARNING'; el.className = 'sensor-badge warning'; }
  else { el.innerText = 'OK'; el.className = 'sensor-badge ok'; }
}

// ── CHARTS ──────────────────────────────────────────────────────
function initCharts() {
  if (!window.Chart) return;
  
  Chart.defaults.color = 'rgba(255,255,255,0.5)';
  Chart.defaults.font.family = "'Inter', sans-serif";
  
  const opts = {
    responsive: true,
    maintainAspectRatio: false,
    animation: { duration: 400 },
    scales: {
      x: { display: false },
      y: { grid: { color: 'rgba(255,255,255,0.05)' }, beginAtZero: false }
    },
    plugins: {
      legend: {
        display: true,
        labels: { boxWidth: 10, font: { size: 10 }, padding: 10 }
      }
    }
  };

  const ctxT = document.getElementById('temp-chart');
  if (ctxT) {
    tChart = new Chart(ctxT.getContext('2d'), {
      type: 'line',
      data: { labels: [], datasets: [
        { label: 'Node 1', borderColor: '#00d4ff', backgroundColor: 'rgba(0,212,255,0.08)', fill: true, tension: 0.4, data: [], pointRadius: 0, borderWidth: 2 },
        { label: 'Node 2', borderColor: '#7b61ff', borderDash: [5, 5], tension: 0.4, data: [], pointRadius: 0, borderWidth: 1.5 }
      ]},
      options: opts
    });
  }

  const ctxH = document.getElementById('hum-chart');
  if (ctxH) {
    hChart = new Chart(ctxH.getContext('2d'), {
      type: 'line',
      data: { labels: [], datasets: [
        { label: 'Node 1', borderColor: '#7b61ff', backgroundColor: 'rgba(123,97,255,0.08)', fill: true, tension: 0.4, data: [], pointRadius: 0, borderWidth: 2 },
        { label: 'Node 2', borderColor: '#00d4ff', borderDash: [5, 5], tension: 0.4, data: [], pointRadius: 0, borderWidth: 1.5 }
      ]},
      options: opts
    });
  }
}

function updateCharts(historyData) {
  if (!tChart || !hChart) return;
  const h1 = historyData['1'] || [];
  const h2 = historyData['2'] || [];
  
  const labels = h1.map(d => d.time);
  
  tChart.data.labels = labels;
  tChart.data.datasets[0].data = h1.map(d => d.temperature);
  tChart.data.datasets[1].data = h2.map(d => d.temperature);
  tChart.update('none');
  
  hChart.data.labels = labels;
  hChart.data.datasets[0].data = h1.map(d => d.humidity);
  hChart.data.datasets[1].data = h2.map(d => d.humidity);
  hChart.update('none');
}
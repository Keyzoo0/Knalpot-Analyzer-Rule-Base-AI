// ============ Tabs ============
document.querySelectorAll('.tab').forEach(t => {
  t.addEventListener('click', () => {
    document.querySelectorAll('.tab').forEach(x => x.classList.remove('active'));
    document.querySelectorAll('.tab-content').forEach(x => x.classList.remove('active'));
    t.classList.add('active');
    document.getElementById('tab-' + t.dataset.tab).classList.add('active');
    if (t.dataset.tab === 'info') fetchInfo();
    if (t.dataset.tab === 'settings') fetchSettings();
    if (t.dataset.tab === 'log') fetchLogs();
  });
});

// ============ State ============
let settings = null;
let flushOn = false;
let latestStatus = null;
let lastServerState = null;

// ============ WebSocket ============
let ws = null;
function connectWS() {
  const proto = location.protocol === 'https:' ? 'wss' : 'ws';
  ws = new WebSocket(`${proto}://${location.host}/ws`);
  ws.onopen = () => {
    const dot = document.getElementById('connDot');
    dot.classList.remove('bg-red-500');
    dot.classList.add('bg-emerald-500','pulse-dot');
    document.getElementById('connText').textContent = 'connected';
  };
  ws.onclose = () => {
    const dot = document.getElementById('connDot');
    dot.classList.add('bg-red-500');
    dot.classList.remove('bg-emerald-500','pulse-dot');
    document.getElementById('connText').textContent = 'disconnected';
    setTimeout(connectWS, 1500);
  };
  ws.onmessage = (ev) => {
    try {
      const m = JSON.parse(ev.data);
      handleMsg(m);
    } catch(e) {}
  };
}
function send(obj){ if (ws && ws.readyState===1) ws.send(JSON.stringify(obj)); }

// ============ Chart ============
const ctx = document.getElementById('chart').getContext('2d');
const gradHC = ctx.createLinearGradient(0, 0, 0, 280);
gradHC.addColorStop(0, 'rgba(37,99,235,0.25)');
gradHC.addColorStop(1, 'rgba(37,99,235,0)');
const gradCO = ctx.createLinearGradient(0, 0, 0, 280);
gradCO.addColorStop(0, 'rgba(220,38,38,0.25)');
gradCO.addColorStop(1, 'rgba(220,38,38,0)');

const chart = new Chart(ctx, {
  type: 'line',
  data: {
    labels: [],
    datasets: [
      { label: 'HC (ppm)', borderColor: '#2563eb', backgroundColor: gradHC, data: [], yAxisID: 'y',  tension: 0.3, pointRadius: 0, borderWidth: 2.5, fill: true },
      { label: 'CO (%)',   borderColor: '#dc2626', backgroundColor: gradCO, data: [], yAxisID: 'y1', tension: 0.3, pointRadius: 0, borderWidth: 2.5, fill: true },
    ]
  },
  options: {
    animation: false,
    responsive: true, maintainAspectRatio: false,
    interaction: { intersect: false, mode: 'index' },
    plugins: { legend: { display: false }, tooltip: { backgroundColor: '#0f172a', titleColor: '#fff', bodyColor: '#cbd5e1', padding: 10, cornerRadius: 8 } },
    scales: {
      y:  { position:'left',  title:{display:true,text:'HC (ppm)', color:'#64748b', font:{size:11}}, grid:{color:'#f1f5f9'}, ticks:{color:'#94a3b8',font:{size:10}} },
      y1: { position:'right', title:{display:true,text:'CO (%)',   color:'#64748b', font:{size:11}}, grid:{drawOnChartArea:false}, ticks:{color:'#94a3b8',font:{size:10}} },
      x:  { ticks: { maxTicksLimit: 6, color:'#94a3b8', font:{size:10} }, grid:{color:'#f1f5f9'} }
    }
  }
});
let chartBufferSize = 200;
function computeBufferSize() {
  if (!settings) return 200;
  const totalMs = settings.inhale_ms + settings.preprocess_ms + settings.sample_interval_ms * settings.sample_count;
  return Math.max(60, Math.ceil(totalMs / 200) + 20);
}
function pushPoint(hc, co) {
  const t = new Date().toLocaleTimeString();
  chart.data.labels.push(t);
  chart.data.datasets[0].data.push(hc);
  chart.data.datasets[1].data.push(co);
  while (chart.data.labels.length > chartBufferSize) {
    chart.data.labels.shift();
    chart.data.datasets[0].data.shift();
    chart.data.datasets[1].data.shift();
  }
  chart.update('none');
}
function clearChart(){
  chart.data.labels = [];
  chart.data.datasets[0].data = [];
  chart.data.datasets[1].data = [];
  chart.update('none');
}

// ============ Phase timeline ============
const PHASE_ORDER = ['INHALE','PREPROCESS','SAMPLING','RESULT'];
function updatePhaseTimeline(state){
  const steps = document.querySelectorAll('.phase-step');
  const curIdx = PHASE_ORDER.indexOf(state);
  steps.forEach((el, i) => {
    el.classList.remove('active','done','pending');
    if (curIdx === -1) el.classList.add('pending');
    else if (i < curIdx) el.classList.add('done');
    else if (i === curIdx) el.classList.add('active');
    else el.classList.add('pending');
  });
}

// ============ Message handler ============
// Sinkronisasi state terpusat. Dipanggil dari pesan "state" (one-shot tiap transisi)
// DAN pesan "data" (periodik 200ms). Karena data membawa snapshot lengkap
// (state + selected_index + flush), UI bisa self-heal walau frame "state" ter-drop
// di antrian WebSocket — web tidak akan lagi "stuck" saat ESP32/TFT sudah lanjut.
function applyServerState(m) {
  const st = m.state;
  document.getElementById('valState').textContent = st;

  // Bersihkan chart HANYA saat transisi MASUK ke INHALE (bukan tiap frame).
  if (st === 'INHALE' && lastServerState !== 'INHALE') clearChart();
  if (st === 'IDLE') document.getElementById('resultBox').classList.add('hidden');

  // Modal pilih index — buka sekali saat transisi masuk SELECT_IDX
  if (st === 'SELECT_IDX' && lastServerState !== 'SELECT_IDX') {
    openIndexModal();
  } else if (st !== 'SELECT_IDX') {
    document.getElementById('modalIndex').classList.add('hidden');
  }
  // Modal valve — buka sekali saat transisi masuk WAIT_VALVE
  if (st === 'WAIT_VALVE' && lastServerState !== 'WAIT_VALVE') {
    document.getElementById('modalValve').classList.remove('hidden');
  } else if (st !== 'WAIT_VALVE') {
    document.getElementById('modalValve').classList.add('hidden');
  }

  // Flush button sync (hanya jika field tersedia)
  if (typeof m.flush === 'boolean') {
    flushOn = m.flush;
    const b = document.getElementById('btnFlush');
    document.getElementById('flushState').textContent = flushOn?'ON':'OFF';
    if (flushOn) {
      b.classList.add('bg-amber-500','text-white','border-amber-500','hover:bg-amber-600');
      b.classList.remove('bg-white','text-ink-700','hover:bg-ink-50','border-ink-200');
    } else {
      b.classList.remove('bg-amber-500','text-white','border-amber-500','hover:bg-amber-600');
      b.classList.add('bg-white','text-ink-700','hover:bg-ink-50','border-ink-200');
    }
  }
  // Label index terpilih (hanya jika field tersedia)
  if (m.selected_index !== undefined) {
    if (m.selected_index >= 0 && settings) {
      document.getElementById('curIdx').textContent = settings.indices[m.selected_index]?.label || '-';
    } else {
      document.getElementById('curIdx').textContent = '-';
    }
  }

  updateButtons(st);
  updatePhaseTimeline(st);
  lastServerState = st;
}

function handleMsg(m) {
  if (m.type === 'data') {
    document.getElementById('valHC').textContent = m.hc.toFixed(0);
    document.getElementById('valCO').textContent = m.co.toFixed(2);
    if (m.phase_total > 0) {
      const pct = Math.min(100, (m.elapsed / m.phase_total) * 100);
      document.getElementById('phaseBar').value = pct;
    } else {
      document.getElementById('phaseBar').value = 0;
    }
    const extra = document.getElementById('valExtra');
    if (m.state === 'SAMPLING') extra.textContent = `Sample ${m.sampled}/${m.sample_target}`;
    else if (m.phase_total > 0) extra.textContent = `${Math.round(m.elapsed/1000)}s / ${Math.round(m.phase_total/1000)}s`;
    else extra.innerHTML = '&nbsp;';
    applyServerState(m);   // self-heal sinkronisasi (modal/tombol/timeline)
    if (['INHALE','PREPROCESS','SAMPLING'].includes(m.state)) pushPoint(m.hc, m.co);
  } else if (m.type === 'state') {
    applyServerState(m);
  } else if (m.type === 'result') {
    showResult(m);
    updateButtons('RESULT');
    updatePhaseTimeline('RESULT');
    // Auto refresh tabel log jika tab Log sedang dibuka
    if (document.getElementById('tab-log').classList.contains('active')) {
      setTimeout(() => fetchLogs && fetchLogs(), 700);   // beri jeda agar SD selesai menulis
    }
  } else if (m.type === 'calib_done') {
    calibDone(m);
  }
}

function updateButtons(state){
  const start = document.getElementById('btnStart');
  const stop  = document.getElementById('btnStop');
  const flush = document.getElementById('btnFlush');
  if (state === 'IDLE') {
    start.classList.remove('hidden');
    stop.classList.add('hidden');
    flush.disabled = false;
    flush.classList.remove('opacity-50','cursor-not-allowed');
  } else if (state === 'RESULT') {
    start.classList.add('hidden');
    stop.classList.add('hidden');
    flush.disabled = true;
    flush.classList.add('opacity-50','cursor-not-allowed');
  } else {
    start.classList.add('hidden');
    stop.classList.remove('hidden');
    flush.disabled = true;
    flush.classList.add('opacity-50','cursor-not-allowed');
  }
}

function showResult(m){
  const box = document.getElementById('resultBox');
  const hero = document.getElementById('resultHero');
  const icon = document.getElementById('resultIcon');
  box.classList.remove('hidden');
  // reset gradient
  hero.className = 'p-6 text-white';
  const palettes = [
    'bg-gradient-to-br from-emerald-500 to-green-600',           // 0 normal
    'bg-gradient-to-br from-amber-500 to-yellow-600',            // 1 gejala
    'bg-gradient-to-br from-orange-500 to-red-500',              // 2 misfire
    'bg-gradient-to-br from-red-600 to-rose-700'                 // 3 tidak normal
  ];
  hero.classList.add(...(palettes[m.code] || 'bg-gradient-to-br from-blue-500 to-indigo-600').split(' '));

  // swap icon
  const icons = [
    '<path stroke-linecap="round" stroke-linejoin="round" d="M9 12l2 2 4-4m6 2a9 9 0 11-18 0 9 9 0 0118 0z"/>',
    '<path stroke-linecap="round" stroke-linejoin="round" d="M12 8v4m0 4h.01M21 12a9 9 0 11-18 0 9 9 0 0118 0z"/>',
    '<path stroke-linecap="round" stroke-linejoin="round" d="M12 9v4m0 4h.01M4.93 19h14.14a2 2 0 001.74-3l-7.07-12a2 2 0 00-3.48 0l-7.07 12a2 2 0 001.74 3z"/>',
    '<path stroke-linecap="round" stroke-linejoin="round" d="M10 14l2-2m0 0l2-2m-2 2l-2-2m2 2l2 2m7-2a9 9 0 11-18 0 9 9 0 0118 0z"/>'
  ];
  icon.innerHTML = icons[m.code] || icons[0];

  document.getElementById('resLabel').textContent = m.label;
  document.getElementById('resHC').textContent = m.avg_hc.toFixed(1);
  document.getElementById('resCO').textContent = m.avg_co.toFixed(2);
  document.getElementById('resThHC').textContent = m.th_hc;
  document.getElementById('resThCO').textContent = m.th_co;
  document.getElementById('resIdx').textContent = m.index_label;
  // status simpan log SD (field log_saved dari firmware; undefined = firmware lama)
  let saveNote = document.getElementById('resLogSaved');
  if (!saveNote) {
    saveNote = document.createElement('div');
    saveNote.id = 'resLogSaved';
    saveNote.className = 'mt-2 text-xs font-medium';
    hero.appendChild(saveNote);
  }
  if (m.log_saved === true) {
    saveNote.textContent = '✓ Tersimpan di data log (SD card)';
    saveNote.style.color = 'rgba(255,255,255,0.9)';
  } else if (m.log_saved === false) {
    saveNote.textContent = '✗ GAGAL simpan ke SD card — cek kartu SD!';
    saveNote.style.color = '#fde047';
  } else {
    saveNote.textContent = '';
  }
  // re-trigger animation
  box.classList.remove('reveal'); void box.offsetWidth; box.classList.add('reveal');
}

// ============ Controls ============
document.getElementById('btnFlush').addEventListener('click', () => {
  flushOn = !flushOn;
  send({cmd: 'flush', on: flushOn});
  const b = document.getElementById('btnFlush');
  document.getElementById('flushState').textContent = flushOn?'ON':'OFF';
  if (flushOn) {
    b.classList.add('bg-amber-500','text-white','border-amber-500','hover:bg-amber-600');
    b.classList.remove('bg-white','text-ink-700','hover:bg-ink-50','border-ink-200');
  } else {
    b.classList.remove('bg-amber-500','text-white','border-amber-500','hover:bg-amber-600');
    b.classList.add('bg-white','text-ink-700','hover:bg-ink-50','border-ink-200');
  }
});
document.getElementById('btnStart').addEventListener('click', async () => {
  if (!settings) await fetchSettings();
  // request server transition to SELECT_IDX; modal auto-open via state message
  send({cmd:'request_start'});
});
document.getElementById('btnStop').addEventListener('click', () => send({cmd:'stop'}));
document.getElementById('btnBackIdle').addEventListener('click', () => {
  send({cmd:'stop'});
  document.getElementById('resultBox').classList.add('hidden');
});
document.getElementById('btnRetest').addEventListener('click', async () => {
  send({cmd:'stop'});
  document.getElementById('resultBox').classList.add('hidden');
  if (!settings) await fetchSettings();
  setTimeout(() => send({cmd:'request_start'}), 250);
});

// ============ Index modal ============
async function openIndexModal(){
  if (!settings) await fetchSettings();
  const list = document.getElementById('idxList');
  list.innerHTML = '';
  if (!settings || !settings.indices || settings.indices.length===0){
    list.innerHTML = '<p class="text-sm text-ink-500">Belum ada index. Tambahkan di tab Settings.</p>';
  } else {
    settings.indices.forEach((e,i) => {
      const b = document.createElement('button');
      b.className = 'w-full text-left p-4 rounded-xl border-2 border-ink-100 hover:border-blue-500 hover:bg-blue-50/50 transition group';
      b.innerHTML = `<div class="flex items-center justify-between">
        <div>
          <div class="font-bold text-ink-900">${e.label}</div>
          <div class="text-xs text-ink-500 mt-1 mono">th HC: ${e.th_hc} ppm &nbsp;·&nbsp; th CO: ${e.th_co} %</div>
        </div>
        <svg class="w-5 h-5 text-ink-300 group-hover:text-blue-500" fill="none" viewBox="0 0 24 24" stroke="currentColor" stroke-width="2"><path stroke-linecap="round" stroke-linejoin="round" d="M9 5l7 7-7 7"/></svg>
      </div>`;
      b.addEventListener('click', () => {
        chartBufferSize = computeBufferSize();
        clearChart();
        // Kirim select_idx → server akan transisi ke WAIT_VALVE → modal valve dibuka via state msg
        send({cmd:'select_idx', index: i});
      });
      list.appendChild(b);
    });
  }
  document.getElementById('modalIndex').classList.remove('hidden');
}
document.getElementById('btnCancelIdx').addEventListener('click', () => {
  document.getElementById('modalIndex').classList.add('hidden');
  send({cmd:'stop'});
});
document.getElementById('btnCancelValve').addEventListener('click', () => {
  document.getElementById('modalValve').classList.add('hidden');
  send({cmd:'stop'});
});
document.getElementById('btnConfirmValve').addEventListener('click', () => {
  document.getElementById('modalValve').classList.add('hidden');
  send({cmd:'confirm_valve'});
});

// ============ Settings ============
async function fetchSettings(){
  const r = await fetch('/api/settings');
  settings = await r.json();
  document.getElementById('inhale_ms').value = settings.inhale_ms;
  document.getElementById('preprocess_ms').value = settings.preprocess_ms;
  document.getElementById('sample_interval_ms').value = settings.sample_interval_ms;
  document.getElementById('sample_count').value = settings.sample_count;
  document.getElementById('r0_mq2').value = settings.r0_mq2;
  document.getElementById('r0_mq7').value = settings.r0_mq7;
  renderIdxTable();
  chartBufferSize = computeBufferSize();
}

function renderIdxTable(){
  const tb = document.getElementById('idxBody');
  tb.innerHTML = '';
  settings.indices.forEach((e, i) => {
    const tr = document.createElement('tr');
    tr.className = 'hover:bg-ink-50/50';
    tr.innerHTML = `
      <td class="px-3 py-2"><input value="${e.label}" data-i="${i}" data-k="label" class="w-full px-3 py-1.5 border border-ink-200 rounded-lg focus:outline-none focus:ring-2 focus:ring-blue-500/30 focus:border-blue-500"></td>
      <td class="px-3 py-2"><input type="number" step="any" value="${e.th_hc}" data-i="${i}" data-k="th_hc" class="w-full px-3 py-1.5 border border-ink-200 rounded-lg focus:outline-none focus:ring-2 focus:ring-blue-500/30 focus:border-blue-500 mono"></td>
      <td class="px-3 py-2"><input type="number" step="any" value="${e.th_co}" data-i="${i}" data-k="th_co" class="w-full px-3 py-1.5 border border-ink-200 rounded-lg focus:outline-none focus:ring-2 focus:ring-blue-500/30 focus:border-blue-500 mono"></td>
      <td class="px-3 py-2 text-right"><button class="inline-flex items-center gap-1 px-3 py-1.5 rounded-lg bg-red-50 hover:bg-red-100 text-red-700 text-xs font-semibold" data-del="${i}">
        <svg class="w-3.5 h-3.5" fill="none" viewBox="0 0 24 24" stroke="currentColor" stroke-width="2"><path stroke-linecap="round" stroke-linejoin="round" d="M19 7l-.867 12.142A2 2 0 0116.138 21H7.862a2 2 0 01-1.995-1.858L5 7m5 4v6m4-6v6M1 7h22M9 7V4a1 1 0 011-1h4a1 1 0 011 1v3"/></svg>
        Hapus
      </button></td>`;
    tb.appendChild(tr);
  });
  tb.querySelectorAll('input').forEach(inp => {
    inp.addEventListener('input', () => {
      const i = +inp.dataset.i, k = inp.dataset.k;
      settings.indices[i][k] = (k==='label') ? inp.value : parseFloat(inp.value)||0;
    });
  });
  tb.querySelectorAll('button[data-del]').forEach(b => {
    b.addEventListener('click', () => {
      settings.indices.splice(+b.dataset.del, 1);
      renderIdxTable();
    });
  });
}

document.getElementById('btnAddIdx').addEventListener('click', () => {
  if (!settings) return;
  settings.indices.push({label:'Index Baru', th_hc:500, th_co:2});
  renderIdxTable();
});

document.getElementById('btnSave').addEventListener('click', async () => {
  settings.inhale_ms = +document.getElementById('inhale_ms').value;
  settings.preprocess_ms = +document.getElementById('preprocess_ms').value;
  settings.sample_interval_ms = +document.getElementById('sample_interval_ms').value;
  settings.sample_count = +document.getElementById('sample_count').value;
  settings.r0_mq2 = parseFloat(document.getElementById('r0_mq2').value);
  settings.r0_mq7 = parseFloat(document.getElementById('r0_mq7').value);
  const r = await fetch('/api/settings', {
    method: 'POST', headers: {'Content-Type':'application/json'},
    body: JSON.stringify(settings)
  });
  const msg = document.getElementById('saveMsg');
  if (r.ok) {
    msg.textContent = '✓ Tersimpan';
    msg.className = 'text-sm font-medium text-emerald-600';
    setTimeout(() => msg.textContent = '', 2000);
    chartBufferSize = computeBufferSize();
  } else {
    msg.textContent = '✗ Gagal menyimpan';
    msg.className = 'text-sm font-medium text-red-600';
  }
});

// Kalibrasi non-blocking: POST hanya memicu start di ESP32 (sampling jalan di loop
// firmware). Hasil datang via WebSocket 'calib_done'; kalau frame ter-drop, ada
// fallback poll GET /api/calibrate.
let calibFallbackTimer = null;

function calibDone(j) {
  const btn = document.getElementById('btnCalibrate');
  const msg = document.getElementById('calibMsg');
  if (calibFallbackTimer) { clearTimeout(calibFallbackTimer); calibFallbackTimer = null; }
  if (j.ok) {
    msg.textContent = `✓ R0 MQ-2=${j.r0_mq2.toFixed(0)}Ω, R0 MQ-7=${j.r0_mq7.toFixed(0)}Ω (target HC=${j.target_hc} ppm, CO=${j.target_co_pct}%)`;
    msg.className = 'text-xs font-medium text-emerald-700';
    document.getElementById('r0_mq2').value = j.r0_mq2.toFixed(1);
    document.getElementById('r0_mq7').value = j.r0_mq7.toFixed(1);
  } else {
    msg.textContent = '✗ ' + (j.err || 'gagal');
    msg.className = 'text-xs font-medium text-red-600';
  }
  btn.disabled = false;
  setTimeout(() => { msg.textContent = ''; }, 10000);
}

async function calibPollFallback() {
  calibFallbackTimer = null;
  try {
    const r = await fetch('/api/calibrate');
    const j = await r.json();
    if (j.active) {
      // masih jalan (mis. ESP32 sempat sibuk) -> cek lagi 3 detik
      calibFallbackTimer = setTimeout(calibPollFallback, 3000);
    } else if (j.result) {
      calibDone(j.result);
    } else {
      calibDone({ ok: false, err: 'hasil tidak diterima' });
    }
  } catch (e) {
    calibDone({ ok: false, err: 'koneksi gagal' });
  }
}

document.getElementById('btnCalibrate').addEventListener('click', async () => {
  const btn = document.getElementById('btnCalibrate');
  const msg = document.getElementById('calibMsg');
  const tHc = parseFloat(document.getElementById('calibTargetHc').value);
  const tCo = parseFloat(document.getElementById('calibTargetCo').value);
  if (!isFinite(tHc) || tHc < 1 || tHc > 50000) {
    msg.textContent = '✗ target HC invalid (1 - 50000 ppm)';
    msg.className = 'text-xs font-medium text-red-600';
    return;
  }
  if (!isFinite(tCo) || tCo < 0.01 || tCo > 10) {
    msg.textContent = '✗ target CO invalid (0.01 - 10 %)';
    msg.className = 'text-xs font-medium text-red-600';
    return;
  }
  if (!confirm(`Pastikan motor menyala IDLE STABIL dan sensor sudah preheat ≥5 menit.\n\nTarget kalibrasi:\n• HC = ${tHc} ppm\n• CO = ${tCo} %\n\nLanjutkan kalibrasi?`)) return;
  btn.disabled = true;
  msg.textContent = 'Mengukur (~10 detik)...';
  msg.className = 'text-xs font-medium text-amber-700';
  try {
    const r = await fetch(`/api/calibrate?hc=${encodeURIComponent(tHc)}&co=${encodeURIComponent(tCo)}`, { method: 'POST' });
    const j = await r.json();
    if (j.ok && j.started) {
      // hasil menyusul via WS 'calib_done'; fallback poll kalau frame WS ter-drop
      calibFallbackTimer = setTimeout(calibPollFallback, (j.duration_ms || 10000) + 4000);
    } else {
      calibDone({ ok: false, err: j.err || 'gagal start' });
    }
  } catch (e) {
    calibDone({ ok: false, err: 'koneksi gagal' });
  }
});

// ============ Log (data logger SD) ============
let logChart = null;
const RESULT_BADGES = [
  { bg:'bg-emerald-100', tx:'text-emerald-700' },  // 0 normal
  { bg:'bg-amber-100',   tx:'text-amber-700'   },  // 1 gejala
  { bg:'bg-orange-100',  tx:'text-orange-700'  },  // 2 misfire
  { bg:'bg-red-100',     tx:'text-red-700'     },  // 3 tidak normal
];

async function fetchLogs(){
  const body = document.getElementById('logBody');
  body.innerHTML = '<tr><td colspan="9" class="text-center py-6 text-ink-400">Memuat...</td></tr>';
  document.getElementById('logEmpty').classList.add('hidden');
  document.getElementById('logSdWarn').classList.add('hidden');
  try {
    const r = await fetch('/api/logs');
    if (r.status === 503) {
      let err = '';
      try { err = (await r.json()).err || ''; } catch (_) {}
      if (err === 'busy') {
        // SD sedang dipakai task lain (transient) — auto-refresh akan retry
        document.getElementById('logCount').textContent = '— sibuk, mencoba lagi...';
        return;
      }
      document.getElementById('logSdWarn').classList.remove('hidden');
      body.innerHTML = '';
      document.getElementById('logCount').textContent = '— SD tidak aktif';
      return;
    }
    const arr = await r.json();
    document.getElementById('logCount').textContent = `${arr.length} entri`;
    body.innerHTML = '';
    if (arr.length === 0) {
      document.getElementById('logEmpty').classList.remove('hidden');
      return;
    }
    // tampilkan terbaru di atas
    arr.slice().reverse().forEach(e => {
      const bg = RESULT_BADGES[e.code]?.bg || 'bg-ink-100';
      const tx = RESULT_BADGES[e.code]?.tx || 'text-ink-700';
      const tr = document.createElement('tr');
      tr.className = 'hover:bg-ink-50';
      const vehicle = (e.vehicle || '').trim();
      const plate   = (e.plate || '').trim();
      tr.innerHTML = `
        <td class="px-3 py-2 mono text-ink-400">#${e.id}</td>
        <td class="px-3 py-2 mono text-xs">${e.ts || '-'}</td>
        <td class="px-3 py-2">${vehicle ? escapeHtml(vehicle) : '<span class="text-ink-300 italic">—</span>'}</td>
        <td class="px-3 py-2 mono uppercase">${plate ? escapeHtml(plate) : '<span class="text-ink-300 italic">—</span>'}</td>
        <td class="px-3 py-2">${escapeHtml(e.idx || '-')}</td>
        <td class="px-3 py-2 mono text-right">${(+e.avg_hc).toFixed(1)}</td>
        <td class="px-3 py-2 mono text-right">${(+e.avg_co).toFixed(2)}</td>
        <td class="px-3 py-2"><span class="inline-block px-2 py-0.5 rounded text-xs font-semibold ${bg} ${tx}">${escapeHtml(e.label || '-')}</span></td>
        <td class="px-3 py-2 text-right">
          <button data-detail="${e.id}" class="inline-flex items-center gap-1 px-2 py-1 rounded bg-blue-50 hover:bg-blue-100 text-blue-700 text-xs font-semibold">
            Detail
          </button>
        </td>`;
      body.appendChild(tr);
    });
    body.querySelectorAll('button[data-detail]').forEach(b => {
      b.addEventListener('click', () => openLogDetail(+b.dataset.detail));
    });
  } catch(e) {
    body.innerHTML = '<tr><td colspan="9" class="text-center py-6 text-red-500">Gagal memuat log</td></tr>';
  }
}

function escapeHtml(s){
  return String(s).replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
}

let logEditId = null;   // id entry yang sedang dibuka di modal detail

async function openLogDetail(id){
  try {
    const r = await fetch('/api/log?id=' + id);
    if (!r.ok) return alert('Log tidak ditemukan');
    const d = await r.json();
    logEditId = id;
    document.getElementById('logDetailTitle').textContent = d.label || '-';
    document.getElementById('logDetailMeta').textContent =
      `${d.ts || '-'}  ·  ${d.idx_label || '-'}  ·  ${d.n} sample`;
    // Form data kendaraan (editable)
    document.getElementById('logEditVehicle').value = d.vehicle || '';
    document.getElementById('logEditPlate').value   = d.plate || '';
    document.getElementById('logEditKategori').textContent = d.idx_label || '-';
    document.getElementById('logMetaMsg').textContent = '';
    document.getElementById('logDetailHC').textContent = (+d.avg_hc).toFixed(1);
    document.getElementById('logDetailCO').textContent = (+d.avg_co).toFixed(2);
    document.getElementById('logDetailThHC').textContent = d.th_hc;
    document.getElementById('logDetailThCO').textContent = d.th_co;

    const ctxL = document.getElementById('logChart').getContext('2d');
    if (logChart) logChart.destroy();
    const labels = (d.s_hc || []).map((_, i) => i + 1);
    logChart = new Chart(ctxL, {
      type: 'line',
      data: {
        labels,
        datasets: [
          { label: 'HC (ppm)', borderColor:'#2563eb', backgroundColor:'rgba(37,99,235,0.15)', data: d.s_hc || [], yAxisID:'y',  tension:0.3, pointRadius:2, borderWidth:2, fill:true },
          { label: 'CO (%)',   borderColor:'#dc2626', backgroundColor:'rgba(220,38,38,0.15)', data: d.s_co || [], yAxisID:'y1', tension:0.3, pointRadius:2, borderWidth:2, fill:true },
        ]
      },
      options: {
        animation:false, responsive:true, maintainAspectRatio:false,
        plugins: { legend: { display: true, position:'bottom' } },
        scales: {
          y:  { position:'left',  title:{display:true,text:'HC (ppm)'} },
          y1: { position:'right', title:{display:true,text:'CO (%)'}, grid:{drawOnChartArea:false} },
          x:  { title:{display:true,text:'Sample #'} }
        }
      }
    });
    document.getElementById('modalLogDetail').classList.remove('hidden');
  } catch(e) {
    alert('Gagal memuat detail');
  }
}

document.getElementById('btnLogDetailClose').addEventListener('click', () => {
  document.getElementById('modalLogDetail').classList.add('hidden');
});
document.getElementById('btnLogSaveMeta').addEventListener('click', async () => {
  if (logEditId === null) return;
  const btn = document.getElementById('btnLogSaveMeta');
  const msg = document.getElementById('logMetaMsg');
  const vehicle = document.getElementById('logEditVehicle').value.trim();
  const plate   = document.getElementById('logEditPlate').value.trim().toUpperCase();
  btn.disabled = true;
  msg.textContent = 'Menyimpan...';
  msg.className = 'text-sm font-medium text-amber-700';
  try {
    const r = await fetch('/api/log/edit?id=' + logEditId, {
      method: 'POST', headers: {'Content-Type':'application/json'},
      body: JSON.stringify({ vehicle, plate })
    });
    const j = await r.json();
    if (j.ok) {
      msg.textContent = '✓ Tersimpan';
      msg.className = 'text-sm font-medium text-emerald-600';
      fetchLogs();   // refresh tabel agar kolom kendaraan/plat update
    } else {
      msg.textContent = '✗ ' + (j.err || 'gagal');
      msg.className = 'text-sm font-medium text-red-600';
    }
  } catch(e) {
    msg.textContent = '✗ koneksi gagal';
    msg.className = 'text-sm font-medium text-red-600';
  } finally {
    btn.disabled = false;
    setTimeout(() => { msg.textContent = ''; }, 3000);
  }
});
document.getElementById('btnLogRefresh').addEventListener('click', fetchLogs);
document.getElementById('btnLogClear').addEventListener('click', async () => {
  if (!confirm('Hapus SEMUA log? Aksi ini tidak bisa dibatalkan.')) return;
  const r = await fetch('/api/logs', { method: 'DELETE' });
  if (r.ok) fetchLogs();
  else alert('Gagal menghapus');
});

// (Auto-refresh log saat RESULT sudah di-hook dari handleMsg() di atas.)

// ============ Info ============
function setRssiBars(rssi){
  const bars = ['rssiBar1','rssiBar2','rssiBar3','rssiBar4'].map(id => document.getElementById(id));
  let count = 0;
  if (rssi >= -55) count = 4;
  else if (rssi >= -65) count = 3;
  else if (rssi >= -75) count = 2;
  else if (rssi >= -85) count = 1;
  const col = count >= 3 ? 'bg-emerald-500' : count >= 2 ? 'bg-amber-500' : 'bg-red-500';
  bars.forEach((b, i) => {
    b.classList.remove('bg-emerald-500','bg-amber-500','bg-red-500','bg-ink-200');
    b.classList.add(i < count ? col : 'bg-ink-200');
  });
}

async function fetchInfo(){
  try {
    const r = await fetch('/api/status');
    const s = await r.json();
    latestStatus = s;
    document.getElementById('infoRssi').textContent = s.rssi + ' dBm';
    setRssiBars(s.rssi);
    document.getElementById('infoIp').textContent = s.ip;
    setOk('infoMq2', s.mq2_ok, s.mq2_ok ? 'OK' : 'DISCONNECTED');
    setOk('infoMq7', s.mq7_ok, s.mq7_ok ? 'OK' : 'DISCONNECTED');
    setOk('infoMotor', true, s.motor ? 'ACTIVE' : 'IDLE');
    setOk('infoTft', s.tft_ok, s.tft_ok ? 'OK' : 'FAIL');
    setOk('infoSd', !!s.sd_ok, s.sd_ok ? 'OK' : 'NO CARD');
    document.getElementById('infoHeap').textContent = (s.heap/1024).toFixed(1) + ' KB';
    const up = Math.floor(s.uptime_ms/1000);
    const h = Math.floor(up/3600), m = Math.floor((up%3600)/60), se = up%60;
    document.getElementById('infoUptime').textContent = `${h}h ${m}m ${se}s`;
    document.getElementById('infoAdc2').textContent = s.adc_mq2;
    document.getElementById('infoAdc7').textContent = s.adc_mq7;
    document.getElementById('infoHC').textContent = s.hc.toFixed(0);
    document.getElementById('infoCO').textContent = s.co.toFixed(2);
  } catch(e) {}
}
function setOk(id, ok, txt){
  const el = document.getElementById(id);
  el.textContent = txt;
  el.classList.remove('text-emerald-600','text-red-600','text-ink-900');
  el.classList.add(ok ? 'text-emerald-600' : 'text-red-600');
}
document.getElementById('btnRefreshInfo').addEventListener('click', fetchInfo);

// ============ Boot ============
fetchSettings().then(() => { chartBufferSize = computeBufferSize(); });
connectWS();
setInterval(() => { if (document.getElementById('tab-info').classList.contains('active')) fetchInfo(); }, 3000);

// Sources: /api/status, the PSRAM ring (debug builds) and every file under /debug.
// Missing sources are left out. Search filters the loaded text line by line.
const $ = (id) => document.getElementById(id);
// A boot row is a cold boot after power-on; any other reset (OTA, panic, watchdog) is a restart.
const cold = (s) => s.includes('reset=POWERON');
let text = '';

function add(label, url, dl, zp = dl) {
  const o = new Option(label, url);
  o.dataset.dl = dl;
  o.dataset.zp = zp;
  $('src').add(o);
}

async function walk(dir, depth) {
  const r = await fetch('/api/files?path=' + encodeURIComponent(dir)).catch(() => null);
  if (!r || !r.ok) return;
  for (const f of await r.json()) {
    const p = dir + '/' + f.name;
    if (f.isDirectory) {
      if (depth < 3) await walk(p, depth + 1);
    } else if (f.name.toLowerCase() !== 'remote-token') {
      add(p.slice(1) + ' (' + f.size.toLocaleString() + 'B)', '/download?path=' + encodeURIComponent(p), f.name, p.slice(1));
    }
  }
}

function show() {
  const q = $('q').value.toLowerCase();
  const lines = text.split('\n');
  const hits = q ? lines.filter((l) => l.toLowerCase().includes(q)) : lines;
  const kind = $('src').selectedOptions[0]?.dataset.dl || '';
  const esc = (s) => s.replace(/&/g, '&amp;').replace(/</g, '&lt;');
  const wrap = (c, h) => (c ? `<span class="${c}">${h}</span>` : h);
  // Logs: errors and crashes red, DBG dimmed, each [TAG] a stable hue from its name.
  const lvl = (l) => (/\[ERR\]|Guru Meditation|panic|abort\(\)|assert failed/.test(l) ? 'e' : l.includes('[DBG]') ? 'd' : '');
  const hue = (t) => ([...t].reduce((h, c) => (h * 31 + c.charCodeAt(0)) >>> 0, 7) * 137) % 360;
  const tag = (l) => l.replace(/^(\[\s*\d+\] \[\w+\] )\[([^\]]+)\]/, (m, a, t) => `${a}<span style="color:hsl(${hue(t)} 60% var(--tl))">[${t}]</span>`);
  // JSON: keys, strings, numbers, true/false/null.
  const json = (l) =>
    l.replace(/("(?:[^"\\]|\\.)*")(\s*:)?|\b(true|false|null)\b|-?\b\d+(?:\.\d+)?\b/g, (m, s, colon, lit) =>
      s ? wrap(colon ? 'jk' : 'js', s) + (colon || '') : wrap(lit ? 'jb' : 'jn', m));
  // Battery CSV: the row's event picks the chart's colors.
  const ev = (l) => {
    const e = l.split(',')[9] || '';
    return e === 'boot' ? (cold(l) ? 'c-boot' : 'c-rst') : e.startsWith('fw_') ? 'c-fw' : e.startsWith('xfer') ? 'c-xfer'
      : /^(chg_|charged|usb_)/.test(e) ? 'c-usb' : e.startsWith('wifi') ? 'c-wifi' : /^(sleep|wake)$/.test(e) ? 'c-sleep' : '';
  };
  const fmt = kind.endsWith('.json') ? json : kind.endsWith('.csv') ? (l) => wrap(ev(l), l) : (l) => wrap(lvl(l), tag(l));
  $('out').innerHTML = hits.map((l) => fmt(esc(l))).join('\n');
  $('meta').textContent = q ? hits.length + ' of ' + lines.length + ' lines' : lines.length + ' lines';
}

async function load() {
  const o = $('src').selectedOptions[0];
  if (!o || !o.value) return;
  $('dl').href = o.value;
  $('dl').download = o.dataset.dl;
  $('meta').textContent = 'Loading...';
  try {
    const r = await fetch(o.value);
    text = r.ok ? await r.text() : 'HTTP ' + r.status;
    if (o.dataset.dl === 'status.json') text = JSON.stringify(JSON.parse(text), null, 2);
  } catch (e) {
    text = String(e);
  }
  show();
}

(async () => {
  $('src').length = 0;
  add('Device status (/api/status)', '/api/status', 'status.json');
  // since=max returns a one-line reply instead of the whole ring: a cheap probe.
  const p = await fetch('/api/psram-log?since=4294967295').catch(() => null);
  if (p && p.ok) {
    const size = p.headers.get('X-Log-Next') - p.headers.get('X-Log-Oldest');
    add('PSRAM log (' + size.toLocaleString() + 'B)', '/api/psram-log', 'psram-log.txt');
  }
  await walk('/debug', 1);
  load();
})();
$('src').onchange = load;
// Download all: the browser zips every source with the File Manager's JSZip,
// fetching one at a time since the SD card serves one reader.
$('all').onclick = async () => {
  $('all').disabled = true;
  try {
    if (!window.JSZip)
      await new Promise((ok, no) => document.head.append(Object.assign(document.createElement('script'), { src: '/js/jszip.min.js', onload: ok, onerror: no })));
    const zip = new JSZip();
    const srcs = [...$('src').options].map((o) => [o.value, o.dataset.zp]).concat([['/api/battery-pending', 'battery-pending.csv']]);
    for (const [u, name] of srcs) {
      $('meta').textContent = 'Zipping ' + name + '...';
      const r = await fetch(u).catch(() => null);
      if (r && r.ok) zip.file(name, await r.blob());
    }
    const a = document.createElement('a');
    a.href = URL.createObjectURL(await zip.generateAsync({ type: 'blob', compression: 'DEFLATE' }));
    a.download = 'crossdink-logs-' + new Date().toISOString().slice(0, 16).replace(/\D/g, '') + '.zip';
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 10000);
    show();
  } catch (e) {
    $('meta').textContent = 'Zip failed: ' + e;
  }
  $('all').disabled = false;
};
$('q').oninput = show;

// Log text size, remembered in this browser.
let fontPx = 14;
try {
  fontPx = +localStorage.getItem('logFontPx') || 14;
} catch (e) {}
function font(d) {
  fontPx = Math.min(32, Math.max(10, fontPx + d));
  $('out').style.fontSize = fontPx + 'px';
  try {
    localStorage.setItem('logFontPx', fontPx);
  } catch (e) {}
}
$('fm').onclick = () => font(-2);
$('fp').onclick = () => font(2);
font(0);

// Battery: /debug/logs/battery.1.csv + battery.csv + rows still in PSRAM
// (/api/battery-pending), columns as BatteryLog.h, plus live readings and
// refresh counts from /api/status. Rows without an RTC time (epoch 0) are left out.
let bat = [];
let segs = [];
let status = {};

const hrs = (s) => (s < 3600 ? Math.round(s / 60) + 'min' : (s / 3600).toFixed(1) + 'h');
// A logged % is whole ("71", older rows and rows logged asleep) or has the
// gauge's fraction ("71.43"), so a drop is exact to u = ±1 or ±0.01: rates and
// estimates carry that ±, and wait for a 2% drop ('-' until then).
const rate = (drop, s, u = 1) => (drop >= 2 && s >= 60 ? ((drop * 3600) / s).toFixed(2) + '±' + ((u * 3600) / s).toFixed(2) + '%/h' : '-');
const left = (pct, drop, s, u = 1) => (drop >= 2 && s >= 60 ? hrs((pct * s) / drop) + ' ±' + hrs((pct * s * u) / drop / drop) : '-');
const unit = (g) => Math.max(g.a.q, g.b.q);
const NOT_ENOUGH = 'not enough data';
const when = (t) => new Date(t * 1000).toLocaleString([], { month: 'short', day: 'numeric', hour: '2-digit', minute: '2-digit' });
const table = (id, head, rows) => {
  $(id).innerHTML =
    (head ? '<tr>' + head.map((h) => '<th>' + h + '</th>').join('') + '</tr>' : '') +
    rows.map((r) => '<tr>' + r.map((c) => '<td>' + c + '</td>').join('') + '</tr>').join('');
};

// One segment per pair of rows: the state from the first row until the next.
function segments() {
  segs = [];
  let wifi = false;
  let xfer = false; // xfer_start .. xfer_end
  let fw = false; // fw_start .. fw_ok / fw_fail
  for (let i = 0; i + 1 < bat.length; i++) {
    const a = bat[i];
    const b = bat[i + 1];
    const reset = ['boot', 'wake', 'sleep'].includes(a.ev);
    if (a.ev === 'wifi_on') wifi = true;
    if (a.ev === 'wifi_off' || reset) wifi = false;
    if (a.ev === 'xfer_start') xfer = true;
    if (a.ev === 'xfer_end' || reset) xfer = false;
    if (a.ev === 'fw_start') fw = true;
    if (a.ev === 'fw_ok' || a.ev === 'fw_fail' || reset) fw = false;
    const dt = b.t - a.t;
    if (dt < 0) continue; // clock set backwards
    // Charger rows logged while asleep carry detail "asleep" and keep the sleep going.
    const state = a.ev === 'sleep' || a.det === 'asleep' ? 'asleep' : b.ev === 'boot' && cold(b.det) ? 'off' : a.usb ? 'usb' : 'awake';
    segs.push({ a, b, dt, state, xfer, fw, wifi: wifi && state !== 'asleep', light: a.light > 0, batt: !a.usb && !b.usb });
  }
}

// Zoom and pan: wheel or pinch zooms around the pointer, dragging pans, double-click
// or Reset zoom returns to the Range window. All three charts share one view.
const L = 44; // left gutter for the value labels
let view = null; // zoomed [t0, t1]; null = the Range select's window
let raf = 0;
let hoverT = null; // last hovered time, so the crosshair survives a redraw
const ptrs = new Map(); // pointerId -> last clientX
const redraw = () => raf || (raf = requestAnimationFrame(() => ((raf = 0), draw())));
function win() {
  const t1 = bat[bat.length - 1].t;
  const span = +$('range').value;
  return view || [span ? Math.max(bat[0].t, t1 - span) : bat[0].t, t1];
}
function setView(a, b) {
  const lo = bat[0].t;
  const hi = bat[bat.length - 1].t;
  if (b - a < 300) [a, b] = [(a + b) / 2 - 150, (a + b) / 2 + 150]; // 5 min at most zoom
  if (b - a >= hi - lo) [a, b] = [lo, hi];
  if (a < lo) [a, b] = [lo, b + lo - a];
  if (b > hi) [a, b] = [a - (b - hi), hi];
  view = [a, b];
  redraw();
}
// Pointer x -> time, in the chart's own viewBox units.
function tAt(svg, cx) {
  const box = svg.getBoundingClientRect();
  const W = svg.viewBox.baseVal.width;
  const [a, b] = win();
  return a + (((cx - box.left) / box.width) * W - L) * ((b - a) / (W - L - svg.dataset.r));
}
function hover(t) {
  hoverT = t;
  const r = bat.reduce((best, c) => (Math.abs(c.t - t) < Math.abs(best.t - t) ? c : best), bat[0]);
  $('readout').title = $('readout').textContent = `${r.local}  ${r.pct}%  ${r.mv}mV  ${r.temp ?? '-'}°C  light ${r.light}%  ${r.usb ? 'USB ' : ''}${r.chg ? 'charging ' : ''}${bat.filter((c) => c.t === r.t).map((c) => (c.ev + ' ' + c.det).trim()).join(', ')}`; // every event at that second, e.g. wifi_on + xfer_start
  const [a, b] = win();
  for (const svg of document.querySelectorAll('.ch svg')) {
    const W = svg.viewBox.baseVal.width - svg.dataset.r;
    const xh = svg.querySelector('.xh');
    if (xh) xh.setAttribute('transform', `translate(${(L + ((r.t - a) * (W - L)) / Math.max(1, b - a)).toFixed(1)})`);
  }
}

// key2/fmt2: an optional second series on its own right-hand scale.
function chart(id, h, key, lo, hi, fmt, bands, key2, fmt2) {
  const svg = $(id);
  const VW = svg.clientWidth || 1000; // user units = CSS px, so 13px labels stay 13px on phones
  svg.setAttribute('viewBox', `0 0 ${VW} ${h}`);
  svg.dataset.r = key2 ? 40 : 0; // right gutter for the second scale
  const W = VW - svg.dataset.r; // plot's right edge
  const ticks = W < 600 ? 1 : 4;
  const [t0, t1] = win();
  const pts = bat
    .filter((r, i) => (r.t >= t0 || (bat[i + 1] && bat[i + 1].t >= t0)) && (r.t <= t1 || (bat[i - 1] && bat[i - 1].t <= t1)))
    .filter((r) => r[key] != null);
  svg.parentNode.style.display = pts.length ? '' : 'none';
  if (!pts.length) return;
  if (key !== 'pct') {
    lo = Math.min(...pts.map((r) => r[key]));
    hi = Math.max(...pts.map((r) => r[key]));
    if (hi - lo < 1) hi = lo + 1;
  }
  const x = (t) => L + ((Math.min(Math.max(t, t0), t1) - t0) * (W - L)) / Math.max(1, t1 - t0);
  const y = (v) => h - 16 - ((v - lo) * (h - 22)) / (hi - lo);
  let s = '';
  let s2 = ''; // second series, drawn over the bands
  if (key2) {
    const p2 = pts.filter((r) => r[key2] != null);
    const lo2 = Math.min(...p2.map((r) => r[key2]));
    const hi2 = Math.max(lo2 + 1, ...p2.map((r) => r[key2]));
    const y2 = (v) => h - 16 - ((v - lo2) * (h - 22)) / (hi2 - lo2);
    for (let q = 0; q <= 4; q++) s += `<text class="t2" x="${VW}" y="${y2(lo2 + ((hi2 - lo2) * q) / 4) + 4}" text-anchor="end">${fmt2(lo2 + ((hi2 - lo2) * q) / 4)}</text>`;
    s2 = `<polyline class="line2" points="${p2.map((r) => x(r.t).toFixed(1) + ',' + y2(r[key2]).toFixed(1)).join(' ')}"/>`;
  }
  if (bands) {
    for (const g of segs) {
      if (g.b.t < t0 || g.a.t > t1) continue;
      const w = Math.max(1, x(g.b.t) - x(g.a.t)).toFixed(1);
      // Shade awake time like the Goodies graph's bar; charging (awake or asleep) and off win.
      const cls = g.state === 'off' ? 'off' : g.a.chg ? 'charging' : g.state === 'asleep' ? '' : 'awake';
      if (cls) s += `<rect class="${cls}" x="${x(g.a.t).toFixed(1)}" y="6" width="${w}" height="${h - 22}"><title>${cls}${g.state === 'asleep' ? ', asleep' : ''} ${hrs(g.dt)}</title></rect>`;
      for (const k of ['xfer', 'fw']) if (g[k]) s += `<rect class="${k}" x="${x(g.a.t).toFixed(1)}" y="6" width="${w}" height="${h - 22}"><title>${k === 'fw' ? 'firmware update' : 'transfer'} ${hrs(g.dt)}</title></rect>`;
      if (g.wifi) s += `<rect class="wifi" x="${x(g.a.t).toFixed(1)}" y="${h - 20}" width="${w}" height="4"/>`;
    }
    for (const r of bat) {
      if (r.t < t0 || r.t > t1) continue;
      const m = r.ev === 'boot' ? (cold(r.det) ? 'boot' : 'rst') : '';
      if (m) s += `<line class="m-${m}" x1="${x(r.t)}" x2="${x(r.t)}" y1="6" y2="${h - 16}"><title>${r.local} ${r.ev} ${r.det}</title></line>`;
    }
  }
  for (let q = 0; q <= 4; q++) {
    const v = lo + ((hi - lo) * q) / 4;
    s += `<line class="grid" x1="${L}" x2="${W}" y1="${y(v)}" y2="${y(v)}"/><text${key2 ? ' class="t1"' : ''} x="0" y="${y(v) + 4}">${fmt(v)}</text>`;
  }
  for (let q = 0; q <= ticks; q++) {
    const t = t0 + ((t1 - t0) * q) / ticks;
    const anchor = q === 0 ? 'start' : q === ticks ? 'end' : 'middle';
    s += `<text x="${x(t)}" y="${h - 2}" text-anchor="${anchor}">${when(t)}</text>`;
  }
  const P = pts.map((r) => x(r.t).toFixed(1) + ',' + y(r[key]).toFixed(1)).join(' ');
  if (key === 'pct') s += `<polygon class="area" points="${x(pts[0].t).toFixed(1)},${y(lo)} ${P} ${x(pts[pts.length - 1].t).toFixed(1)},${y(lo)}"/>`;
  s += s2 + `<polyline class="line" points="${P}"/><line class="xh" x1="0" x2="0" y1="6" y2="${h - 16}" transform="translate(-9)"/>`;
  svg.innerHTML = s;
  svg.onwheel = (e) => {
    e.preventDefault();
    const tc = tAt(svg, e.clientX);
    const f = e.deltaY > 0 ? 1.25 : 0.8;
    const [a, b] = win();
    setView(tc - (tc - a) * f, tc + (b - tc) * f);
  };
  svg.onpointerdown = (e) => {
    svg.setPointerCapture(e.pointerId);
    ptrs.set(e.pointerId, e.clientX);
  };
  svg.onpointermove = (e) => {
    hover(tAt(svg, e.clientX));
    if (!ptrs.has(e.pointerId)) return;
    const [a, b] = win();
    const other = [...ptrs].find(([id]) => id !== e.pointerId);
    if (!other) {
      const dt = ((ptrs.get(e.pointerId) - e.clientX) * (b - a)) / ((svg.getBoundingClientRect().width * (W - L)) / VW);
      setView(a + dt, b + dt);
    } else {
      const d0 = Math.abs(ptrs.get(e.pointerId) - other[1]);
      const d1 = Math.abs(e.clientX - other[1]);
      const tc = tAt(svg, (e.clientX + other[1]) / 2);
      if (d0 > 10 && d1 > 10) setView(tc - ((tc - a) * d0) / d1, tc + ((b - tc) * d0) / d1);
    }
    ptrs.set(e.pointerId, e.clientX);
  };
  svg.onpointerup = svg.onpointercancel = (e) => ptrs.delete(e.pointerId);
  svg.ondblclick = () => ((view = null), draw());
}

function draw() {
  $('rz').hidden = !view;
  chart('gp', 220, 'pct', 0, 100, (v) => Math.round(v) + '%', true, 'mv', (v) => Math.round(v) + '');
  chart('gt', 120, 'temp', 0, 0, (v) => v.toFixed(1) + '°', false);
  if (hoverT !== null) hover(hoverT);
}

function stateTable() {
  const by = {};
  for (const g of segs) {
    if (!g.batt || g.state === 'off') continue;
    const k = g.state === 'asleep' ? 'asleep' : 'awake' + (g.wifi ? ', Wi-Fi on' : '') + (g.light ? ', light on' : '');
    const o = (by[k] = by[k] || { s: 0, drop: 0, u: 0.01 });
    o.s += g.dt;
    o.drop += Math.max(0, g.a.pct - g.b.pct);
    o.u = Math.max(o.u, unit(g));
  }
  const pct = status.battery ? status.battery.percent : bat[bat.length - 1].pct;
  table(
    'states',
    ['State', 'Time', 'Drop', 'Rate', `Runtime from ${pct}%`],
    Object.entries(by).map(([k, o]) => [k, hrs(o.s), +o.drop.toFixed(2) + '%', rate(o.drop, o.s, o.u), left(pct, o.drop, o.s, o.u)])
  );
}

function sessions() {
  const dis = [];
  const chg = [];
  let cur = null;
  for (const g of segs) {
    const kind = g.a.usb ? 'chg' : 'dis';
    if (!cur || cur.kind !== kind) {
      cur = { kind, a: g.a, b: g.b, awake: 0, asleep: 0, dropAwake: 0, dropAsleep: 0, full: null, max: g.a.pct, u: 0.01 };
      (kind === 'chg' ? chg : dis).push(cur);
    }
    cur.b = g.b;
    cur.max = Math.max(cur.max, g.b.pct);
    cur.u = Math.max(cur.u, unit(g));
    const d = Math.max(0, g.a.pct - g.b.pct);
    if (g.state === 'off') continue;
    if (g.state === 'asleep') {
      cur.asleep += g.dt;
      cur.dropAsleep += d;
    } else {
      cur.awake += g.dt;
      cur.dropAwake += d;
    }
    if (g.b.ev === 'charged' && !cur.full) cur.full = g.b.t;
  }
  table(
    'dis',
    ['From', 'Span', '%', 'Awake', 'Asleep', 'Overall'],
    dis.reverse().map((s) => [
      when(s.a.t),
      hrs(s.b.t - s.a.t),
      s.a.pct + ' → ' + s.b.pct,
      hrs(s.awake) + ', ' + rate(s.dropAwake, s.awake, s.u),
      hrs(s.asleep) + ', ' + rate(s.dropAsleep, s.asleep, s.u),
      rate(s.dropAwake + s.dropAsleep, s.awake + s.asleep, s.u),
    ])
  );
  table(
    'chg',
    ['From', 'Span', '%', 'Speed', 'Full after'],
    chg.reverse().map((s) => [
      when(s.a.t),
      hrs(s.b.t - s.a.t),
      s.a.pct + ' → ' + s.max,
      rate(s.max - s.a.pct, s.b.t - s.a.t, s.u).replace('%/h', '%/h gained'),
      s.full ? hrs(s.full - s.a.t) : '-',
    ])
  );
}

// The Summary counters, counted from the log like Goodies > Battery & stats does
// (BatteryStatsActivity::parseRow): from the last stats_reset row, or the first row.
function logStats() {
  const zero = (reset) => ({ reset, first: 0, last: 0, cold: 0, rst: 0, wakes: 0, falseWakes: 0, awake: 0, asleep: 0, charged: null, bA: 0, bS: 0, dA: 0, dS: 0, u: 0.01 });
  let s = zero(false);
  let prev = null;
  for (const r of bat) {
    const c = r.ev === 'boot' && cold(r.det);
    if (r.ev === 'stats_reset') s = zero(true);
    else if (s.first && prev && r.t >= prev.t && !c) {
      // The span from the previous row is awake or asleep; before a cold boot it was off.
      const dt = r.t - prev.t;
      const drop = Math.max(0, prev.pct - r.pct);
      const awake = !(prev.ev === 'sleep' || prev.det.startsWith('asleep'));
      s[awake ? 'awake' : 'asleep'] += dt;
      if (!prev.usb && !r.usb) {
        awake ? ((s.bA += dt), (s.dA += drop)) : ((s.bS += dt), (s.dS += drop));
        s.u = Math.max(s.u, prev.q, r.q);
      }
    }
    s.first ||= r.t;
    s.last = r.t;
    if (r.ev === 'boot') c ? s.cold++ : s.rst++;
    if (r.ev === 'wake') s.wakes++;
    const f = /false_wakes=(\d+)/.exec(r.det);
    if (f) s.falseWakes += +f[1];
    if (r.ev === 'charged') s.charged = r;
    if (r.ev === 'charged' || r.ev === 'chg_off') (s.bA = s.bS = s.dA = s.dS = 0), (s.u = 0.01);
    prev = r;
  }
  return s;
}

function summary() {
  const b = status.battery || {};
  const t = status.temperatures || {};
  const c = (k) => (t[k] && t[k].c != null ? t[k].c + '°C' : '-');
  $('hero').innerHTML = [
    [(b.percent ?? '-') + '%', b.charging ? 'charging' : b.usb ? 'on USB' : 'on battery'],
    [(b.millivolts ?? '-') + 'mV', 'voltage'],
    [c('battery'), 'battery temp'],
  ].map(([v, l]) => `<div><b>${v}</b><span>${l}</span></div>`).join('');
  const rows = [
    ['Now', `${b.percent ?? '-'}%  ${b.millivolts ?? '-'}mV  ${b.charging ? 'charging ' : ''}${b.usb ? 'USB' : 'on battery'}`],
    ['Temperatures', `battery ${c('battery')}, chip ${c('chip')}, panel ${c('panel')}`],
  ];
  if (bat.length) {
    const s = logStats();
    const now = (b.stats && b.stats.now) || Date.now() / 1000;
    const pct = b.percent ?? bat[bat.length - 1].pct;
    const drop = s.dA + s.dS;
    const span = s.bA + s.bS;
    rows.push(
      ['Last charged', s.charged && now > s.charged.t ? `${hrs(now - s.charged.t)} ago at ${s.charged.pct}%` : 'not in the log'],
      ['Awake drain', s.dA >= 2 && s.bA >= 60 ? rate(s.dA, s.bA, s.u) + ' over ' + hrs(s.bA) : NOT_ENOUGH],
      ['Asleep drain', s.dS >= 2 && s.bS >= 60 ? rate(s.dS, s.bS, s.u) + ' over ' + hrs(s.bS) : NOT_ENOUGH],
      ['Est. left at that pace', drop >= 2 && span >= 60 ? left(pct, drop, span, s.u) : NOT_ENOUGH],
      ['Wakes / false wakes', `${s.wakes} / ${s.falseWakes}`],
      ['Cold boots / restarts', `${s.cold} / ${s.rst}`],
      ['Awake / asleep', hrs(s.awake) + ' / ' + hrs(s.asleep)],
      ['Log', `${bat.length} rows, ${when(bat[0].t)} to ${when(bat[bat.length - 1].t)}` + (s.reset ? `, counters since reset ${when(s.first)}` : '')]
    );
  } else rows.push(['Log', 'no rows with a clock time']);
  if (b.stats && b.stats.refresh) rows.push(['Refreshes since power-on', Object.entries(b.stats.refresh).map(([k, v]) => k + ' ' + v).join(', ')]);
  const boot = status.boot || {};
  rows.push(['Up', `${hrs((boot.uptimeMs || 0) / 1000)}, reset ${boot.resetReason}, wake ${boot.wakeCause}`]);
  table('sum', null, rows);
}

// Battery and Logs are two nav tabs on one page: /logs#battery and /logs.
// A build without battery data falls back to the log viewer.
let hasBat = null; // null until the battery data has loaded
function tab() {
  const b = location.hash === '#battery' && hasBat !== false;
  $('bat').hidden = !(b && hasBat);
  $('lg').hidden = b;
  for (const a of document.querySelectorAll('.nav-links a')) a.classList.toggle('active', a.getAttribute('href') === (b ? '/logs#battery' : '/logs'));
  if (b && bat.length > 1) draw(); // charts size to the card, so draw once it is visible
}
window.onhashchange = tab;
tab();

(async () => {
  const get = async (u) => {
    const r = await fetch(u).catch(() => null);
    return r && r.ok ? r.text() : '';
  };
  const text = [
    await get('/download?path=' + encodeURIComponent('/debug/logs/battery.1.csv')),
    await get('/download?path=' + encodeURIComponent('/debug/logs/battery.csv')),
    await get('/api/battery-pending'),
  ].join('\n');
  status = JSON.parse((await get('/api/status')) || '{}');
  bat = text
    .split('\n')
    .map((l) => l.split(','))
    .filter((f) => f.length >= 10 && +f[0] > 0)
    .map((f) => ({
      t: +f[0], local: f[1], pct: +f[3], q: f[3].includes('.') ? 0.01 : 1, mv: +f[4], chg: f[5] === '1', usb: f[6] === '1',
      temp: f[7] === '' ? null : +f[7], light: +f[8], ev: f[9], det: f.slice(10).join(','),
    }));
  hasBat = bat.length > 0 || !!(status.battery && status.battery.stats);
  if (hasBat) summary();
  if (bat.length > 1) {
    segments();
    stateTable();
    sessions();
    $('range').onchange = () => ((view = null), draw());
    $('rz').onclick = () => ((view = null), draw());
    window.onresize = () => $('bat').hidden || draw();
  }
  tab();
})();

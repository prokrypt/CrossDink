// Sources: /api/status, the PSRAM ring (debug builds) and every file under /debug.
// Missing sources are left out. Search filters the loaded text line by line.
const $ = (id) => document.getElementById(id);
let text = '';

function add(label, url, dl) {
  const o = new Option(label, url);
  o.dataset.dl = dl;
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
      add(p.slice(1) + ' (' + f.size.toLocaleString() + ' B)', '/download?path=' + encodeURIComponent(p), f.name);
    }
  }
}

function show() {
  const q = $('q').value.toLowerCase();
  const lines = text.split('\n');
  const hits = q ? lines.filter((l) => l.toLowerCase().includes(q)) : lines;
  $('out').textContent = hits.join('\n');
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
  if (p && p.ok) add('PSRAM log', '/api/psram-log', 'psram-log.txt');
  await walk('/debug', 1);
  load();
})();
$('src').onchange = load;
$('q').oninput = show;
$('q').onkeydown = (e) => e.key === 'Enter' && show();
$('go').onclick = show;

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
// (/api/battery-pending), columns as BatteryLog.h, plus the Goodies counters
// from /api/status. Rows without an RTC time (epoch 0) are left out.
let bat = [];
let segs = [];
let status = {};

const hrs = (s) => (s < 3600 ? Math.round(s / 60) + ' min' : (s / 3600).toFixed(1) + ' h');
const rate = (drop, s) => (s >= 60 ? ((drop * 3600) / s).toFixed(2) + ' %/h' : '-');
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
  for (let i = 0; i + 1 < bat.length; i++) {
    const a = bat[i];
    const b = bat[i + 1];
    if (a.ev === 'wifi_on') wifi = true;
    if (['wifi_off', 'boot', 'wake', 'sleep'].includes(a.ev)) wifi = false;
    const dt = b.t - a.t;
    if (dt < 0) continue; // clock set backwards
    const state = a.ev === 'sleep' ? 'asleep' : b.ev === 'boot' ? 'off' : a.usb ? 'usb' : 'awake';
    segs.push({ a, b, dt, state, wifi: wifi && state !== 'asleep', light: a.light > 0, batt: !a.usb && !b.usb });
  }
}

function chart(id, h, key, lo, hi, fmt, bands) {
  const svg = $(id);
  const W = svg.clientWidth || 1000; // user units = CSS px, so 13px labels stay 13px on phones
  svg.setAttribute('viewBox', `0 0 ${W} ${h}`);
  const ticks = W < 600 ? 1 : 4;
  const L = 44;
  const span = +$('range').value;
  const t1 = bat[bat.length - 1].t;
  const t0 = span ? Math.max(bat[0].t, t1 - span) : bat[0].t;
  const pts = bat.filter((r, i) => r.t >= t0 || (bat[i + 1] && bat[i + 1].t >= t0)).filter((r) => r[key] != null);
  svg.style.display = pts.length ? '' : 'none';
  if (!pts.length) return;
  if (key !== 'pct') {
    lo = Math.min(...pts.map((r) => r[key]));
    hi = Math.max(...pts.map((r) => r[key]));
    if (hi - lo < 1) hi = lo + 1;
  }
  const x = (t) => L + ((Math.max(t, t0) - t0) * (W - L)) / Math.max(1, t1 - t0);
  const y = (v) => h - 16 - ((v - lo) * (h - 22)) / (hi - lo);
  let s = '';
  if (bands) {
    for (const g of segs) {
      if (g.b.t < t0) continue;
      const w = Math.max(1, x(g.b.t) - x(g.a.t)).toFixed(1);
      const cls = g.state === 'awake' ? '' : g.state;
      if (cls) s += `<rect class="${cls}" x="${x(g.a.t).toFixed(1)}" y="6" width="${w}" height="${h - 22}"><title>${g.state} ${hrs(g.dt)}</title></rect>`;
      if (g.wifi) s += `<rect class="wifi" x="${x(g.a.t).toFixed(1)}" y="${h - 20}" width="${w}" height="4"/>`;
    }
    for (const r of bat) {
      if (r.t < t0) continue;
      const m = r.ev === 'boot' ? 'boot' : r.ev.startsWith('fw_') ? 'fw' : r.ev.startsWith('xfer') ? 'xfer' : '';
      if (m) s += `<line class="m-${m}" x1="${x(r.t)}" x2="${x(r.t)}" y1="6" y2="${h - 16}"><title>${r.local} ${r.ev} ${r.det}</title></line>`;
    }
  }
  for (let q = 0; q <= 4; q++) {
    const v = lo + ((hi - lo) * q) / 4;
    s += `<line class="grid" x1="${L}" x2="${W}" y1="${y(v)}" y2="${y(v)}"/><text x="0" y="${y(v) + 4}">${fmt(v)}</text>`;
  }
  for (let q = 0; q <= ticks; q++) {
    const t = t0 + ((t1 - t0) * q) / ticks;
    const anchor = q === 0 ? 'start' : q === ticks ? 'end' : 'middle';
    s += `<text x="${x(t)}" y="${h - 2}" text-anchor="${anchor}">${when(t)}</text>`;
  }
  s += `<polyline class="line" points="${pts.map((r) => x(r.t).toFixed(1) + ',' + y(r[key]).toFixed(1)).join(' ')}"/>`;
  svg.innerHTML = s;
  svg.onmousemove = (e) => {
    const box = svg.getBoundingClientRect();
    const t = t0 + (((e.clientX - box.left) / box.width) * W - L) * (Math.max(1, t1 - t0) / (W - L));
    const r = bat.reduce((best, c) => (Math.abs(c.t - t) < Math.abs(best.t - t) ? c : best), bat[0]);
    $('readout').textContent = `${r.local}  ${r.pct}%  ${r.mv} mV  ${r.temp ?? '-'} C  light ${r.light}%  ${r.usb ? 'USB ' : ''}${r.chg ? 'charging ' : ''}${r.ev} ${r.det}`;
  };
}

function draw() {
  chart('gp', 220, 'pct', 0, 100, (v) => Math.round(v) + '%', true);
  chart('gv', 120, 'mv', 0, 0, (v) => Math.round(v) + '', false);
  chart('gt', 120, 'temp', 0, 0, (v) => v.toFixed(1) + ' C', false);
}

function stateTable() {
  const by = {};
  for (const g of segs) {
    if (!g.batt || g.state === 'off') continue;
    const k = g.state === 'asleep' ? 'asleep' : 'awake' + (g.wifi ? ', Wi-Fi on' : '') + (g.light ? ', light on' : '');
    const o = (by[k] = by[k] || { s: 0, drop: 0 });
    o.s += g.dt;
    o.drop += Math.max(0, g.a.pct - g.b.pct);
  }
  const pct = status.battery ? status.battery.percent : bat[bat.length - 1].pct;
  table(
    'states',
    ['State', 'Time', 'Drop', 'Rate', `Runtime from ${pct}%`],
    Object.entries(by).map(([k, o]) => [k, hrs(o.s), o.drop + '%', rate(o.drop, o.s), o.drop ? hrs((pct * o.s) / o.drop) : '-'])
  );
}

function sessions() {
  const dis = [];
  const chg = [];
  let cur = null;
  for (const g of segs) {
    const kind = g.a.usb ? 'chg' : 'dis';
    if (!cur || cur.kind !== kind) {
      cur = { kind, a: g.a, b: g.b, awake: 0, asleep: 0, dropAwake: 0, dropAsleep: 0, full: null, max: g.a.pct };
      (kind === 'chg' ? chg : dis).push(cur);
    }
    cur.b = g.b;
    cur.max = Math.max(cur.max, g.b.pct);
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
      hrs(s.awake) + ', ' + rate(s.dropAwake, s.awake),
      hrs(s.asleep) + ', ' + rate(s.dropAsleep, s.asleep),
      rate(s.dropAwake + s.dropAsleep, s.awake + s.asleep),
    ])
  );
  table(
    'chg',
    ['From', 'Span', '%', 'Speed', 'Full after'],
    chg.reverse().map((s) => [
      when(s.a.t),
      hrs(s.b.t - s.a.t),
      s.a.pct + ' → ' + s.max,
      rate(s.max - s.a.pct, s.b.t - s.a.t).replace(' %/h', ' %/h gained'),
      s.full ? hrs(s.full - s.a.t) : '-',
    ])
  );
}

function summary() {
  const b = status.battery || {};
  const st = b.stats || {};
  const t = status.temperatures || {};
  const c = (k) => (t[k] && t[k].c != null ? t[k].c + ' C' : '-');
  const rows = [
    ['Now', `${b.percent ?? '-'}%  ${b.millivolts ?? '-'} mV  ${b.charging ? 'charging ' : ''}${b.usb ? 'USB' : 'on battery'}`],
    ['Temperatures', `battery ${c('battery')}, chip ${c('chip')}, panel ${c('panel')}`],
  ];
  if (st.now != null) {
    const drop = st.dropAwakePct + st.dropAsleepPct;
    const span = st.battAwakeS + st.battAsleepS;
    rows.push(
      ['Unplugged', st.unplugEpoch && st.now > st.unplugEpoch ? `${hrs(st.now - st.unplugEpoch)} ago at ${st.unplugPct}%` : 'not seen since reset'],
      ['Awake drain', rate(st.dropAwakePct, st.battAwakeS) + ' over ' + hrs(st.battAwakeS)],
      ['Asleep drain', rate(st.dropAsleepPct, st.battAsleepS) + ' over ' + hrs(st.battAsleepS)],
      ['Est. left at that pace', drop > 0 && span >= 60 ? hrs((b.percent * span) / drop) : '-'],
      ['Wakes / boots', st.wakes + ' / ' + st.boots],
      ['Awake / asleep', hrs(st.awakeS) + ' / ' + hrs(st.asleepS)],
      ['Refreshes', Object.entries(st.refresh || {}).map(([k, v]) => k + ' ' + v).join(', ')]
    );
  }
  const boot = status.boot || {};
  rows.push(
    ['Up', `${hrs((boot.uptimeMs || 0) / 1000)}, reset ${boot.resetReason}, wake ${boot.wakeCause}`],
    ['Log', bat.length ? `${bat.length} rows, ${when(bat[0].t)} to ${when(bat[bat.length - 1].t)}, ${bat.filter((r) => r.ev === 'boot').length} boots, ${bat.filter((r) => r.ev === 'wake').length} wakes` : 'no rows with a clock time']
  );
  table('sum', null, rows);
}

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
      t: +f[0], local: f[1], pct: +f[3], mv: +f[4], chg: f[5] === '1', usb: f[6] === '1',
      temp: f[7] === '' ? null : +f[7], light: +f[8], ev: f[9], det: f.slice(10).join(','),
    }));
  if (!bat.length && !(status.battery && status.battery.stats)) return;
  $('bat').hidden = false;
  summary();
  if (bat.length < 2) return;
  segments();
  draw();
  stateTable();
  sessions();
  $('range').onchange = draw;
  window.onresize = draw;
})();

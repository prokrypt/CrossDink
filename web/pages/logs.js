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

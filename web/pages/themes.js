// Theme packs: list and pick through /api/themes, upload with the file manager's /mkdir and /upload.
const ROOT = '/themes';
let active = '';

function setStatus(text, ok) {
  const s = document.getElementById('status');
  s.textContent = text;
  s.style.color = ok === false ? '#b91c1c' : '';
}

// Built with textContent: pack names and problems come from the card.
function render(data) {
  active = data.active || '';
  const packs = document.getElementById('packs');
  packs.replaceChildren();
  const rows = [''].concat(data.packs || []);
  for (const name of rows) {
    const row = document.createElement('div');
    row.className = 'pack';
    const label = document.createElement('span');
    label.className = 'pack-name';
    label.textContent = name || 'None (built-in theme)';
    row.appendChild(label);
    if (name === active) {
      const state = document.createElement('span');
      state.className = 'pack-meta';
      state.textContent = name && !data.loaded ? 'Picked, not loaded' : 'In use';
      row.appendChild(state);
    }
    const btn = document.createElement('button');
    btn.className = 'btn btn-small';
    btn.textContent = name === active && name ? 'Reload' : 'Use';
    btn.disabled = name === active && !name;
    btn.addEventListener('click', () => pick(name));
    row.appendChild(btn);
    packs.appendChild(row);
  }
  const list = document.getElementById('problems');
  list.replaceChildren();
  const lines = (data.problems || '').split('\n').filter(Boolean);
  if (lines.length === 0) {
    const li = document.createElement('li');
    li.className = 'empty';
    li.textContent = active ? 'None' : 'No pack picked';
    list.appendChild(li);
  }
  for (const line of lines) {
    const li = document.createElement('li');
    li.textContent = line;
    list.appendChild(li);
  }
}

async function load() {
  try {
    const res = await fetch('/api/themes');
    if (!res.ok) throw new Error('HTTP ' + res.status);
    render(await res.json());
  } catch (e) {
    document.getElementById('packs').textContent = 'Failed to load theme packs';
  }
}

async function pick(name) {
  setStatus(name ? 'Loading ' + name + '...' : 'Turning packs off...');
  try {
    const res = await fetch('/api/themes/pick', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({name})
    });
    if (!res.ok) throw new Error(await res.text());
    const data = await res.json();
    render(data);
    setStatus(!name ? 'Built-in theme in use.' : data.loaded ? name + ' in use.' : name + ' did not load; see Problems.',
              !name || data.loaded);
  } catch (e) {
    setStatus('Pick failed: ' + e.message, false);
  }
}

async function post(url, body) {
  const res = await fetch(url, {method: 'POST', body});
  return {ok: res.ok, text: await res.text()};
}

function form(fields) {
  const f = new FormData();
  for (const [k, v] of Object.entries(fields)) f.append(k, v);
  return f;
}

document.getElementById('uploadForm').addEventListener('submit', async function(e) {
  e.preventDefault();
  const files = Array.from(document.getElementById('packFiles').files).filter(f => !f.name.startsWith('.'));
  const parts = files.map(f => (f.webkitRelativePath || f.name).split('/'));
  const pack = parts.length ? parts[0][0] : '';
  if (!pack || parts.some(p => p.length < 2 || p[0] !== pack)) {
    setStatus('Pick one pack folder.', false);
    return;
  }
  if (!parts.some(p => p.length === 2 && p[1] === 'theme.json')) {
    setStatus(pack + ' has no theme.json at its top.', false);
    return;
  }
  // Folders first, parents before children; "already exists" is fine.
  const dirs = new Set([ROOT, ROOT + '/' + pack]);
  for (const p of parts) {
    for (let i = 2; i < p.length; i++) dirs.add(ROOT + '/' + p.slice(0, i).join('/'));
  }
  for (const dir of [...dirs].sort((a, b) => a.split('/').length - b.split('/').length)) {
    const cut = dir.lastIndexOf('/');
    await post('/mkdir', form({name: dir.slice(cut + 1), path: dir.slice(0, cut) || '/'}));
  }
  for (let i = 0; i < files.length; i++) {
    const dir = ROOT + '/' + parts[i].slice(0, -1).join('/');
    const path = dir + '/' + files[i].name;
    setStatus('Uploading ' + (i + 1) + '/' + files.length + ': ' + path);
    // Uploads refuse to overwrite, so an edited file replaces the old one this way.
    await post('/delete', new URLSearchParams({path}));
    const up = await post('/upload?path=' + encodeURIComponent(dir), form({file: files[i]}));
    if (!up.ok) {
      setStatus('Failed on ' + path + ': ' + up.text, false);
      await load();
      return;
    }
  }
  setStatus('Uploaded ' + files.length + ' file' + (files.length === 1 ? '' : 's') + ' to ' + ROOT + '/' + pack + '.', true);
  // The picked pack reloads so the edit shows; another pack waits for Use.
  if (pack === active) await pick(pack);
  else await load();
});

load();

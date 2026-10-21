// Run with: node --test scripts/test_web_files.cjs
// FILES_PAGE_SCRIPT may point at the script extracted from the generated page.
// Browser APIs are faked at the boundary; the complete portal script and upload
// orchestration run unchanged. This does not exercise device storage or layout.
const assert = require('node:assert/strict');
const { readFileSync } = require('node:fs');
const { join } = require('node:path');
const { File } = require('node:buffer');
const vm = require('node:vm');
const { test } = require('node:test');

const source = readFileSync(process.env.FILES_PAGE_SCRIPT || join(__dirname, '../web/pages/files.js'), 'utf8');

class Element {
  constructor(id) {
    this.id = id;
    this.listeners = new Map();
    const classes = new Set();
    this.classList = {
      add: (...names) => names.forEach(name => classes.add(name)),
      remove: (...names) => names.forEach(name => classes.delete(name)),
      contains: name => classes.has(name),
      toggle: (name, enabled = !classes.has(name)) => enabled ? classes.add(name) : classes.delete(name),
    };
    this.style = {};
    this.dataset = {};
    this.value = '';
    this.textContent = '';
    this.files = [];
    this.checked = false;
  }
  addEventListener(type, callback) {
    if (!this.listeners.has(type)) this.listeners.set(type, []);
    this.listeners.get(type).push(callback);
  }
  emit(type, target = this, extra = {}) {
    const event = { target, preventDefault() {}, ...extra };
    for (const callback of this.listeners.get(type) || []) callback(event);
  }
  removeAttribute(name) { delete this[name]; }
  focus() {}
  select() {}
  setSelectionRange() {}
  querySelectorAll() { return []; }
}

function portal({ entries = [], fallback = false } = {}) {
  const elements = new Map();
  const element = id => {
    if (!elements.has(id)) elements.set(id, new Element(id));
    return elements.get(id);
  };
  const overlays = ['uploadModal', 'folderModal', 'deleteModal', 'renameModal', 'moveModal', 'imagePreviewModal'].map(element);
  const document = new Element('document');
  document.getElementById = element;
  document.querySelectorAll = selector => selector === '.modal-overlay' ? overlays : [];
  document.querySelector = selector => selector === '.modal-overlay.open'
    ? overlays.find(overlay => overlay.classList.contains('open'))
    : element(selector);
  const uploads = [];
  const requests = [];
  const logs = [];
  const storage = new Map();
  class WebSocket {
    static OPEN = 1;
    constructor(url) {
      this.url = url;
      this.readyState = 1;
      this.bufferedAmount = 0;
      queueMicrotask(() => fallback ? this.onerror({}) : this.onopen());
    }
    send(message) {
      if (typeof message === 'string') {
        const [, name, size, path] = message.split(':');
        this.upload = { transport: 'ws', name, size: Number(size), path, chunks: [] };
        uploads.push(this.upload);
        queueMicrotask(() => this.onmessage({ data: 'READY' }));
      } else {
        this.upload.chunks.push(Buffer.from(message));
        if (Buffer.concat(this.upload.chunks).length === this.upload.size) {
          queueMicrotask(() => this.onmessage({ data: 'DONE' }));
        }
      }
    }
    close() { this.readyState = 3; this.onclose({ code: 1000, reason: '' }); }
  }
  class XMLHttpRequest {
    constructor() { this.upload = {}; }
    open(method, url) { this.method = method; this.url = url; }
    send(form) {
      const file = form.get('file');
      uploads.push({ transport: 'http', file, name: file.name, url: this.url });
      this.status = 200;
      queueMicrotask(() => this.onload());
    }
    abort() { this.onabort(); }
  }
  const context = vm.createContext({
    document, navigator: { onLine: true },
    window: { location: { search: '?path=%2Fbooks', hostname: 'reader.test', port: '', reload() {} }, addEventListener() {} },
    URLSearchParams, File, Blob, FormData, WebSocket, XMLHttpRequest,
    console: { log: (...args) => logs.push(args.join(' ')), warn() {}, error() {} },
    alert: message => { throw new Error(message); },
    localStorage: { getItem: key => storage.get(key), setItem: (key, value) => storage.set(key, value), removeItem: key => storage.delete(key) },
    fetch: async url => {
      requests.push(url);
      return { ok: true, json: async () => url.startsWith('/api/files') ? entries : {} };
    },
    // Complete short transfer delays; do not execute the post-upload reload.
    setTimeout: (callback, delay) => { if (delay < 1000) queueMicrotask(callback); return 1; },
    clearTimeout() {},
  });
  // The test controls when hydration occurs, after installing its fixtures.
  assert.match(source, /hydrate\(\);?\s*$/);
  vm.runInContext(source.replace(/hydrate\(\);?\s*$/, ''), context);
  return { context, element, overlays, document, uploads, requests, logs, storage,
    run: code => vm.runInContext(code, context) };
}

async function finishUpload(page) {
  await page.context.uploadFile();
  for (let i = 0; i < 100; i++) {
    if (page.element('progress-text').textContent === 'All uploads complete!') return;
    await new Promise(resolve => setImmediate(resolve));
  }
  assert.fail(`Upload did not finish: ${page.element('progress-text').textContent}`);
}

test('dialog drags and inside interactions stay open; actual backdrop clicks clean up', async () => {
  const page = portal();
  await page.context.hydrate();
  for (const overlay of page.overlays) {
    overlay.classList.add('open');
    const inside = new Element('dialog text');
    overlay.emit('mousedown', inside);
    overlay.emit('click', overlay);
    assert.ok(overlay.classList.contains('open'), `${overlay.id}: selection drag closed dialog`);
    overlay.emit('mousedown', inside);
    overlay.emit('click', inside);
    assert.ok(overlay.classList.contains('open'), `${overlay.id}: inside click closed dialog`);
    overlay.emit('mousedown');
    overlay.emit('click', inside);
    assert.ok(overlay.classList.contains('open'), `${overlay.id}: backdrop-to-inside drag closed dialog`);
    overlay.emit('mousedown');
    overlay.emit('click');
    assert.equal(overlay.classList.contains('open'), false, `${overlay.id}: backdrop click failed`);
    overlay.classList.add('open');
    // A prior close must not leave a stale mousedown target when reopened.
    overlay.emit('click');
    assert.ok(overlay.classList.contains('open'), `${overlay.id}: stale target dismissed reopened dialog`);
    overlay.classList.remove('open');
  }
});

test('backdrop and close icon cannot interrupt uploads; Cancel and Escape still cancel', async () => {
  const page = portal();
  await page.context.hydrate();
  const overlay = page.element('uploadModal');
  overlay.classList.add('open');
  page.run('isUploadInProgress = true');
  let aborts = 0;
  page.context.activeXhr = { abort() { aborts++; } };
  page.run('currentUploadXhr = activeXhr');
  overlay.emit('mousedown');
  overlay.emit('click');
  assert.ok(overlay.classList.contains('open'));
  assert.equal(page.run('operationCancelled'), false);
  assert.equal(aborts, 0);
  page.context.closeUploadModal();
  assert.ok(overlay.classList.contains('open'));
  assert.equal(aborts, 0);
  page.context.handleCancelUploadModal();
  assert.equal(page.run('operationCancelled'), true);
  assert.equal(aborts, 1);
  assert.ok(overlay.classList.contains('open'));
  page.run('operationCancelled = false; currentUploadXhr = activeXhr');
  page.document.emit('keydown', page.document, { key: 'Escape' });
  assert.equal(page.run('operationCancelled'), true);
  assert.equal(aborts, 2);
  page.run('isUploadInProgress = false');
  page.context.closeUploadModal();
  assert.equal(overlay.classList.contains('open'), false);
});

test('normal close, Cancel and Escape still close idle dialogs', async () => {
  const page = portal();
  await page.context.hydrate();
  for (const name of ['closeUploadModal', 'handleCancelUploadModal']) {
    page.element('uploadModal').classList.add('open');
    page.context[name]();
    assert.equal(page.element('uploadModal').classList.contains('open'), false);
  }
  for (const overlay of page.overlays) {
    overlay.classList.add('open');
    page.document.emit('keydown', page.document, { key: 'Escape' });
    assert.equal(overlay.classList.contains('open'), false, overlay.id);
  }
});

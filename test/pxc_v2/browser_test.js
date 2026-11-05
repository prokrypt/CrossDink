// Exercise the real browser encoders without requiring canvas or DOM setup.
const fs = require('node:fs');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const path = require('node:path');
const source = fs.readFileSync(path.join(__dirname, '../../web/pages/files.js'), 'utf8');
vm.runInThisContext(source.slice(source.indexOf('function crossDinkCrc32('), source.indexOf('async function buildCrossDinkPxc(')));
const golden = (name) => new Uint8Array(fs.readFileSync(path.join(__dirname, 'golden', name)));
const output = encodeCrossDinkPxc2(golden('pixels.pxc'));
assert.deepEqual(output, golden('browser.pxc2'));
assert.deepEqual(output, golden('packbits.pxc2'));
for (const count of [0, 1, 89, 256]) {
  const entries = Array.from({ length: count }, (_, i) => ({ href: `EPUB/${i}.jpg`, pxc: 'META-INF/crossink/pxc/x.pxc2',
    width: 127, height: 131, pxcBytes: output.length, pixelCrc32: new DataView(output.buffer).getUint32(24, true) }));
  const manifest = new TextEncoder().encode(JSON.stringify({ images: entries }));
  const index = buildCrossDinkImageIndex(manifest, entries), view = new DataView(index.buffer);
  assert.equal(index.length, 32 + 208 * count);
  assert.equal(view.getUint32(16, true), crossDinkCrc32(manifest));
  assert.equal(view.getUint32(24, true), crossDinkCrc32(index.subarray(32)));
  assert.equal(view.getUint32(28, true), crossDinkCrc32(index.subarray(0, 28)));
  if (count) assert.throws(() => buildCrossDinkImageIndex(manifest, [entries[0], entries[0]]));
}
for (const bad of ['', '/x', '../x', 'a\\b', 'a%00', 'x'.repeat(129)]) assert.ok(!crossDinkIndexPath(bad, 128));
console.log('Browser PXC2 golden and COIX tests passed');

vm.runInThisContext(source.slice(source.indexOf('function crossDinkPxcRules('), source.indexOf('function crossDinkPxcLength(')));
const rules = crossDinkPxcRules('div.full img { width:100px; height:200px }');
const image = (inside) => ({ getAttribute: () => '', matches: (selector) => inside && selector === 'div.full img' });
assert.deepEqual(crossDinkPxcStyle(rules, image(true)), { width: '100px', height: '200px' });
assert.deepEqual(crossDinkPxcStyle(rules, image(false)), {});
assert.deepEqual(crossDinkPxcStyle(rules, { getAttribute: () => '', matches: () => { throw new Error('Invalid selector'); } }), {});

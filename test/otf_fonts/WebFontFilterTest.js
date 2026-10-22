const fs = require('fs');
const vm = require('vm');
const script = fs.readFileSync(process.argv[2], 'utf8');
const elements = new Map();
const context = vm.createContext({
  document: { getElementById(id) {
    if (!elements.has(id)) elements.set(id, {addEventListener() {}, replaceChildren() {}, appendChild() {}});
    return elements.get(id);
  }, createElement() { return {}; } },
  fetch: async () => ({json: async () => ({ttfSupported: false, families: []})})
});
vm.runInContext(script, context);
vm.runInContext(`
  ttfSupported = true;
  if (familyFromFilename('Bitter-BoldItalic.OTF') !== 'Bitter') throw Error('OTF family');
  if (cpfontFilesOnly([{name:'A.OtF'}, {name:'A.TTF'}, {name:'A_14.cpfont'}, {name:'A.ttc'}, {name:'A.otf.tmp'}]).length !== 3)
    throw Error('scalable filter');
  ttfSupported = false;
  if (cpfontFilesOnly([{name:'A.OTF'}, {name:'A.ttf'}, {name:'A_14.cpfont'}]).length !== 1)
    throw Error('C3 gate');
`, context);
console.log('PASS: web OTF family derivation, suffix filtering and C3 gating');

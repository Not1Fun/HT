const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const root = path.join(__dirname, '..');
const ui = JSON.parse(fs.readFileSync(path.join(root, 'ui.json'), 'utf8'));
let now = 0, tick;

class Element {
  constructor(tag = 'div') {
    this.tag = tag; this.style = {}; this.dataset = {}; this.children = [];
    this.attrs = {}; this.listeners = {}; this.classList = {add() {}}; this.clientWidth = 800;
  }
  setAttribute(key, value) { this.attrs[key] = value; }
  append(node) { this.children.push(node); }
  replaceChildren() { this.children = []; }
  addEventListener(kind, handler) { this.listeners[kind] = handler; }
  querySelector() { return this.children.find(node => node.dataset.field === 'elapsed')?.children[0] || null; }
}

const ids = Object.fromEntries(['screen', 'feedback', 'page-label', 'ccw', 'cw', 'press', 'up', 'left', 'ok',
  'right', 'down', 'battery-alarm', 'temperature-invalid', 'bench-disabled'].map(id => [id, new Element()]));
const scenes = ['idle', 'fault', 'offline'].map(scene => Object.assign(new Element(), {dataset: {scene}}));
const document = {
  querySelector: selector => selector === '.screen-stage' ? new Element() : ids[selector.slice(1)],
  querySelectorAll: () => scenes,
  createElement: tag => new Element(tag), createElementNS: (_, tag) => new Element(tag)
};
const context = {document, performance: {now: () => now}, fetch: async () => ({ok: true, json: async () => ui}),
  ResizeObserver: class { observe() {} }, setInterval: callback => { tick = callback; }, console};
function click(id) { ids[id].listeners.click(); }
function checked(id, value) { ids[id].listeners.change({target: {checked: value}}); }
function scene(value) { scenes.find(node => node.dataset.scene === value).listeners.click(); }
function icon(name) {
  const item = ui.icons.find(item => item.name === name);
  const node = ids.screen.children.find(node => node.tag === 'img' && node.style.left === item.x + 'px' && node.style.top === item.y + 'px');
  return Number(/(\d+)\.png/.exec(node.src)[1]) - item.first;
}
function output(value) { assert.equal(ids.screen.dataset.output, String(value)); }
function matching(value) { assert.equal(ids.screen.dataset.matching, String(value)); }
function page(value) { assert.equal(ids.screen.dataset.page, String(value)); }
function advance(ms) { now += ms; tick(); }
function text(name) { return ids.screen.children.find(node => node.dataset.field === name)?.children[0].textContent; }
function field(index) {
  click('up'); click('up'); click('up');
  for (let count = 0; count < index; count++) click('down');
  assert.equal(icon('focus'), index);
}
function start() { field(2); click('cw'); click('ok'); output(true); matching(true); assert.equal(icon('state'), 2); }

(async () => {
  assert.equal(ui.version, 9);
  assert.deepEqual(ui.icons.find(item => item.name === 'focus').values, ['frequency', 'power', 'output']);
  assert.equal(ui.fields.some(item => item.vp === 0x1150 || item.name === 'range_choice'), false);
  for (const [name, vp] of Object.entries({current: 0x1100, voltage: 0x1110, range: 0x1120, frequency: 0x1130,
    elapsed: 0x1140, frequency_choice: 0x1160, ntc1: 0x1170, ntc2: 0x1180, ntc3: 0x1190, power_choice: 0x11a0})) {
    assert.equal(ui.fields.find(item => item.name === name).vp, vp);
  }
  await vm.runInNewContext(fs.readFileSync(path.join(root, 'preview/preview.js'), 'utf8'), context);
  output(false); matching(false); page(0); click('right'); page(1); assert.equal(icon('output'), 3);
  field(2); click('down'); assert.equal(icon('focus'), 2); click('cw'); click('ok'); output(false);
  field(0); click('ccw'); assert.equal(text('frequency_choice'), '10'); click('cw'); assert.equal(text('frequency_choice'), '2');
  field(1); assert.equal(text('power_choice'), '0'); click('ccw'); assert.equal(text('power_choice'), '0');
  for (let count = 0; count < 55; count++) click('cw');
  assert.equal(text('power_choice'), '50'); click('press');
  field(2); click('cw'); assert.equal(icon('output'), 1); output(false); click('ok'); output(true); matching(true);
  click('left'); page(0); assert.equal(text('range'), '1'); assert.equal(text('current'), '--');
  advance(800); assert.equal(text('range'), '1'); matching(true);
  click('right'); click('right'); page(2); advance(800); output(true); matching(true); assert.equal(icon('state'), 2);
  assert.equal(icon('log_event_0'), 16);
  click('left'); click('left'); page(0); assert.equal(text('range'), '30'); advance(800);
  matching(false); output(true); assert.equal(icon('state'), 1); assert.equal(text('range'), '30');
  assert.equal(text('current'), '1.291'); assert.equal(text('voltage'), '38.730');
  advance(65000); assert.equal(text('elapsed'), '00:01:07'); output(true);
  click('down'); output(false); matching(false); click('right'); field(2); click('press'); output(false);
  start(); click('press'); output(false); advance(5000); output(false); matching(false);
  click('cw'); click('left'); click('right'); assert.equal(icon('output'), 0); output(false);
  start(); field(1); click('ccw'); click('ok'); output(false); matching(false); assert.match(ids.feedback.textContent, /49 VA/);
  advance(5000); output(false); start(); field(0); click('cw'); click('ok'); output(false); matching(false);
  start(); advance(2400); matching(false); field(1); click('ok'); output(false);
  start(); checked('temperature-invalid', true); output(false); matching(false); assert.equal(icon('output'), 3);
  checked('temperature-invalid', false); start(); scene('fault'); output(false); matching(false); assert.equal(icon('output'), 4);
  scene('idle'); start(); scene('offline'); output(false); assert.equal(icon('output'), 3); scene('idle');
  checked('bench-disabled', true); assert.equal(icon('output'), 5); click('cw'); click('ok'); output(false);
  checked('bench-disabled', false); field(1); click('ccw'); click('left'); click('right'); assert.equal(text('power_choice'), '49');
  start(); click('left'); click('down'); output(false); advance(5000); output(false); matching(false);
  click('right');
  for (const item of ui.fields.filter(item => item.pages.includes(1))) assert.notEqual(text(item.name), undefined);
  click('right'); click('right'); page(3); assert.equal(text('debug_coils'), '00000000');
  click('ccw'); assert.equal(text('debug_relay'),'K8 1000R'); click('press');
  assert.equal(text('debug_coils'),'10000001'); advance(30001); assert.equal(text('debug_coils'),'00000000');
  click('cw'); click('cw'); click('cw'); assert.equal(text('debug_relay'),'K2 1R'); click('press');
  for(let i=0;i<3;i++) click('down'); click('cw'); click('press');
  assert.equal(text('debug_wave'),'ON'); advance(10001); assert.equal(text('debug_wave'),'OFF');
  click('cw'); click('press'); assert.equal(text('debug_wave'),'ON'); click('right');page(4);
  click('left');page(3);assert.equal(text('debug_coils'),'00000000'); assert.equal(text('debug_wave'),'OFF');
  click('right'); page(4); checked('temperature-invalid',true);
  assert.equal(icon('reason'),0);
  scene('fault'); const first=icon('reason'); advance(2000); const second=icon('reason');
  assert.deepEqual([first,second].sort((a,b)=>a-b),[11,12]);
  scene('idle'); click('right'); click('right'); page(1);
  assert.equal(icon('reason'),18); checked('temperature-invalid',false);
  assert.equal(icon('reason'),0);
  console.log('PASS: Bench relay/wave navigation, timeout/leave cancellation; v9 reason rotation and DAC NTC exemption; VP contract, three fields, frequency wrap, VA bounds/draft, explicit start, matching transitions, page continuity, stop cancellation, continuous over 60s, parameter confirmation, fault/NTC/offline/disabled.');
})().catch(error => { console.error(error); process.exitCode = 1; });

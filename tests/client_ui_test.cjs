const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync('core/web_server/app.js', 'utf8');
function page() {
  const nodes = new Map();
  function element() {
    return { hidden: false, textContent: '', style: {}, dataset: {},
      classList: { add() {}, remove() {}, toggle() {} },
      addEventListener() {}, removeAttribute() {}, load() {}, pause() {},
      appendChild() {}, querySelector: () => element() };
  }
  const document = {
    querySelector(selector) {
      if (!nodes.has(selector)) nodes.set(selector, element());
      return nodes.get(selector);
    },
    querySelectorAll: () => [], createElement: () => element()
  };
  const calls = [], events = [];
  let replaced = false;
  const context = vm.createContext({
    document, window: {}, console, URL, AbortController, performance,
    setInterval: () => 1, clearInterval() {}, setTimeout: () => 1, clearTimeout() {},
    requestAnimationFrame: () => 1, cancelAnimationFrame() {},
    EventSource: class {
      constructor(url) { this.url = url; this.handlers = {}; events.push(this); }
      addEventListener(name, handler) { this.handlers[name] = handler; }
      close() { this.closed = true; }
    },
    fetch: async (path, options = {}) => {
      calls.push({ path, options });
      const status = replaced && path !== '/api/client' ? 409 : 200;
      const payload = path === '/api/client' ? { client: 'page-token' }
        : status === 409 ? { error: 'session_replaced' }
        : path === '/api/plugins' ? [] : {};
      return { status, ok: status === 200,
        headers: { get: () => 'application/json' }, json: async () => payload };
    }
  });
  vm.runInContext(source, context);
  return { nodes, calls, events, context, replace: () => { replaced = true; } };
}
(async () => {
  const first = page();
  await new Promise(setImmediate);
  assert.equal(first.calls.filter(c => c.path === '/api/client').length, 1);
  assert.equal(first.events[0].url, '/api/events?client=page-token');
  assert.equal(first.calls.find(c => c.path === '/api/camera/status').options.headers['X-Vision-Client'], 'page-token');
  vm.runInContext('applyRecording({recording:true, elapsed_seconds:2})', first.context);
  assert.equal(first.nodes.get('#camera-toggle').checked, false);
  assert.equal(first.nodes.get('#record-toggle-btn').disabled, false);
  first.events[0].handlers.session_replaced();
  assert.equal(first.events[0].closed, true);
  assert.equal(first.nodes.get('#client-message').hidden, false);
  assert.equal(first.nodes.get('#view-video').hidden, true);
  vm.runInContext("showView('video'); reconnectStream(); startDashboardEvents();", first.context);
  assert.equal(first.nodes.get('#view-video').hidden, true);
  await assert.rejects(vm.runInContext("request('/api/camera/start', {method:'POST'})", first.context), /Session closed/);
  assert.equal(first.calls.filter(c => c.path === '/api/client').length, 1);
  const second = page();
  await new Promise(setImmediate);
  second.replace();
  second.events[0].onerror();
  await new Promise(setImmediate);
  assert.equal(second.events[0].closed, true);
  assert.equal(second.calls.filter(c => c.path === '/api/client').length, 1);
  const third = page();
  await new Promise(setImmediate);
  await vm.runInContext('pumpH264Stream({status:409}, new AbortController())', third.context);
  assert.equal(third.events[0].closed, true);
  console.log('Client UI takeover and reconnect regression tests passed');
})().catch(error => { console.error(error); process.exitCode = 1; });

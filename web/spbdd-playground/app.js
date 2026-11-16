// SPBDD playground: edit a C++ program against SPBDD and run it with the
// interpreter in cpp.js, over the WebAssembly build of the library, in a worker.

(() => {
  'use strict';

  // Each example is a complete C++ program that also compiles with
  // g++ -std=c++17 prog.cpp libspbdd.a, with the same output.
  const EXAMPLES = [
    ['01-sets.cpp', 'Sets and products'],
    ['02-weights.cpp', 'Weights and patterns'],
    ['03-gates.cpp', 'Clifford gates and measurement'],
    ['04-faults.cpp', 'Faults through a circuit'],
    ['05-code.cpp', 'The [[5,1,3]] code'],
    ['06-qec-condition.cpp', 'QEC condition, d = 3 surface code'],
  ];

  const STORE_KEY = 'spbdd-playground-cpp-v1';
  const $ = id => document.getElementById(id);
  const editor = $('editor'), sel = $('example'), out = $('console');
  const runBtn = $('run'), stopBtn = $('stop'), statusEl = $('status');
  let worker = null, ready = false, running = false, timer = null;
  const sources = new Map();

  function stored() { try { return JSON.parse(localStorage.getItem(STORE_KEY) || '{}'); } catch (_) { return {}; } }
  function store() {
    try { localStorage.setItem(STORE_KEY, JSON.stringify({ example: sel.value, code: editor.value })); } catch (_) { /* ignore */ }
  }

  function loadExample(file) {
    if (sources.has(file)) return Promise.resolve(sources.get(file));
    return fetch('examples/' + file).then(r => {
      if (!r.ok) throw new Error(`${file}: HTTP ${r.status}`);
      return r.text();
    }).then(text => { sources.set(file, text); return text; });
  }

  function showExample(file) {
    loadExample(file).then(text => { editor.value = text; store(); })
      .catch(err => { line('could not load the example: ' + err.message, 'err'); });
  }

  for (const [file, title] of EXAMPLES) sel.append(new Option(title, file));
  const saved = stored();
  if (saved.example && EXAMPLES.some(([f]) => f === saved.example)) sel.value = saved.example;
  if (saved.code) editor.value = saved.code;
  else showExample(sel.value);

  sel.addEventListener('change', () => showExample(sel.value));
  $('reset').addEventListener('click', () => showExample(sel.value));
  editor.addEventListener('input', store);
  editor.addEventListener('keydown', e => {
    if (e.key === 'Tab' && !e.shiftKey && !e.ctrlKey && !e.metaKey) {
      e.preventDefault();
      editor.setRangeText('    ', editor.selectionStart, editor.selectionEnd, 'end');
    }
    if ((e.ctrlKey || e.metaKey) && e.key === 'Enter') { e.preventDefault(); if (!runBtn.disabled) run(); }
  });

  function line(text, cls) {
    const span = document.createElement('span');
    span.textContent = text + '\n';
    if (cls) span.className = cls;
    out.append(span);
    out.scrollTop = out.scrollHeight;
  }

  // Put the caret on the line an error points at.
  function goToLine(n) {
    const lines = editor.value.split('\n');
    let pos = 0;
    for (let i = 0; i < Math.min(n - 1, lines.length); i++) pos += lines[i].length + 1;
    editor.focus();
    editor.setSelectionRange(pos, pos + (lines[n - 1] || '').length);
  }

  function spawn() {
    ready = false;
    worker = new Worker('worker.js');
    worker.onmessage = ({ data }) => {
      if (data.type === 'ready') {
        ready = true;
        if (statusEl.textContent.startsWith('Loading')) statusEl.textContent = 'Ready';
        update();
      } else if (data.type === 'out') {
        line(data.text, data.err ? 'err' : '');
        if (data.line) goToLine(data.line);
      } else if (data.type === 'done') {
        line(`\n[${data.ok ? 'exit ' + data.rc : 'stopped by an error'}, ${data.ms.toFixed(1)} ms]`, 'sys');
        finish(data.ok ? 'Done' : 'Finished with an error');
      }
    };
    worker.onerror = e => { line('error: ' + (e.message || 'SPBDD failed to load'), 'err'); finish('Failed'); };
  }

  function update() { runBtn.disabled = running || !ready; }

  function run() {
    out.textContent = '';
    running = true; update(); stopBtn.disabled = false;
    const started = performance.now();
    statusEl.className = 'status running';
    const tick = () => { statusEl.textContent = `Running, ${((performance.now() - started) / 1000).toFixed(0)} s`; };
    tick(); timer = setInterval(tick, 1000);
    worker.postMessage({ code: editor.value });
  }

  function finish(message) {
    clearInterval(timer);
    running = false; stopBtn.disabled = true;
    statusEl.className = 'status'; statusEl.textContent = message;
    if (worker) worker.terminate();
    spawn(); update();
  }

  runBtn.addEventListener('click', run);
  stopBtn.addEventListener('click', () => { if (running) { line('\n[stopped]', 'sys'); finish('Stopped'); } });
  spawn();
})();

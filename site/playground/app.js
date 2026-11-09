// Verifier playground: pick or write a protocol, edit its files, run the
// WebAssembly build of ftec-verify in a worker.

(() => {
  'use strict';

  // In the browser the verifier runs about 2.6 times slower than natively
  // (CB18 [[17,1,5]]: 121 s native, 310 s in WebAssembly).
  const WASM_SLOWDOWN = 2.6;
  const STORE_KEY = 'fpv-playground-v1';

  const $ = id => document.getElementById(id);
  const plist = $('plist'), tabs = $('tabs'), editor = $('editor'), entrySel = $('entry');
  const runBtn = $('run'), stopBtn = $('stop'), statusEl = $('status'), out = $('console');

  let bundled = [];                 // from protocols.json
  const workspaces = new Map();     // name -> { files, entry, custom, origin }
  let current = null;               // workspace name
  let currentFile = null;
  let worker = null, workerReady = false, running = false, timer = null;

  // --- persistence (best effort; the page works without it) --------------------
  function load() {
    try { return JSON.parse(localStorage.getItem(STORE_KEY) || '{}'); } catch (_) { return {}; }
  }
  function save() {
    const data = {};
    for (const [name, ws] of workspaces) {
      if (ws.custom || ws.edited) data[name] = { files: ws.files, entry: ws.entry, custom: ws.custom };
    }
    try { localStorage.setItem(STORE_KEY, JSON.stringify(data)); } catch (_) { /* full or blocked */ }
  }

  // --- helpers ---------------------------------------------------------------------
  function human(seconds) {
    if (seconds < 1) return 'under a second';
    if (seconds < 90) return `about ${Math.round(seconds)} s`;
    if (seconds < 5400) return `about ${Math.round(seconds / 60)} min`;
    return `about ${(seconds / 3600).toFixed(1)} h`;
  }
  function verdictOf(p) {
    if (!p.expected) return null;
    const failures = /failures=(\d+)/.exec(p.expected);
    const minT = /min_t=(-?\d+)/.exec(p.expected);
    if (!failures) return null;
    return failures[1] === '0' ? { good: true, text: 'fault tolerant' }
                               : { good: false, text: `fails at t = ${minT ? minT[1] : '?'}` };
  }
  function codeOf(files, entry) {
    const m = /\[\[\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\]\]/.exec(files[entry] || '');
    return m ? `[[${m[1]},${m[2]},${m[3]}]]` : '';
  }
  function el(tag, props, ...children) {
    const e = document.createElement(tag);
    Object.assign(e, props || {});
    for (const c of children) e.append(c);
    return e;
  }

  // --- protocol list -------------------------------------------------------------
  function renderList() {
    plist.textContent = '';
    const groups = [
      ['Distance 3, instant', bundled.filter(p => p.d <= 3)],
      ['Distance 5 and 7, minutes to hours', bundled.filter(p => p.d > 3)],
    ];
    for (const [title, items] of groups) {
      plist.append(el('h3', { textContent: title }));
      for (const p of items) plist.append(listButton(p.name, p));
    }
    const mine = [...workspaces.values()].filter(ws => ws.custom);
    plist.append(el('h3', { textContent: 'Your protocols' }));
    if (!mine.length) {
      plist.append(el('p', { className: 'notice', textContent: 'Choose New or Add files to bring your own .fpdl and .qasm files.' }));
    }
    for (const ws of mine) plist.append(listButton(ws.name, null));
  }

  function listButton(name, p) {
    const ws = workspaces.get(name);
    const b = el('button', { type: 'button' });
    b.setAttribute('aria-pressed', String(name === current));
    b.append(name);
    const meta = el('span', { className: 'meta' });
    if (p) {
      meta.append(`[[${p.n},${p.k},${p.d}]]`);
      const v = verdictOf(p);
      if (v) { meta.append(', '); meta.append(el('span', { className: v.good ? 'good' : 'bad', textContent: v.text })); }
      if (p.native_seconds === null) meta.append(', over 2000 s natively');
      else if (p.native_seconds >= 1) meta.append(`, ${human(p.native_seconds * WASM_SLOWDOWN)} here`);
      if (ws && ws.edited) meta.append(', edited');
    } else {
      meta.append(codeOf(ws.files, ws.entry) || 'your files');
    }
    b.append(meta);
    b.addEventListener('click', () => select(name));
    return b;
  }

  // --- workspace -----------------------------------------------------------------
  function select(name) {
    flushEditor();
    current = name;
    const ws = workspaces.get(name);
    const names = Object.keys(ws.files);
    currentFile = names.includes(ws.entry) ? ws.entry : names[0] || null;
    $('ws-title').textContent = name;
    $('reset').hidden = ws.custom;
    renderList();
    renderTabs();
    renderEntry();
    renderEstimate();
    try { history.replaceState(null, '', '?p=' + encodeURIComponent(name)); } catch (_) { /* file:// */ }
  }

  function renderTabs() {
    const ws = workspaces.get(current);
    tabs.textContent = '';
    const names = Object.keys(ws.files).sort((a, b) =>
      (b.endsWith('.fpdl') - a.endsWith('.fpdl')) || a.localeCompare(b));
    for (const name of names) {
      const t = el('button', { type: 'button', title: 'Double-click to rename' });
      t.setAttribute('role', 'tab');
      t.setAttribute('aria-selected', String(name === currentFile));
      t.append(name);
      const x = el('span', { className: 'close', textContent: '×', title: 'Remove ' + name });
      x.setAttribute('aria-label', 'Remove ' + name);
      x.addEventListener('click', ev => { ev.stopPropagation(); removeFile(name); });
      t.append(x);
      t.addEventListener('click', () => { flushEditor(); currentFile = name; renderTabs(); });
      t.addEventListener('dblclick', () => renameFile(t, name));
      tabs.append(t);
    }
    editor.value = currentFile ? ws.files[currentFile] : '';
    editor.disabled = !currentFile;
    editor.placeholder = currentFile ? '' : 'No files yet. Add files or create a new one.';
  }

  function renderEntry() {
    const ws = workspaces.get(current);
    entrySel.textContent = '';
    const fpdls = Object.keys(ws.files).filter(n => n.endsWith('.fpdl')).sort();
    for (const n of fpdls) entrySel.append(el('option', { value: n, textContent: n }));
    if (!fpdls.includes(ws.entry)) ws.entry = fpdls[0] || null;
    if (ws.entry) entrySel.value = ws.entry;
    updateRunButton();
  }

  function renderEstimate() {
    const p = bundled.find(b => b.name === current);
    const note = $('estimate');
    note.className = 'notice';
    if (!p) { note.textContent = 'Files stay in this browser. The .fpdl file’s relative paths resolve against the files listed here.'; return; }
    if (p.native_seconds === null) {
      note.className = 'notice warn';
      note.textContent = 'This protocol did not finish within 2000 s natively. It will very likely run out of time or memory here.';
    } else if (p.native_seconds >= 60) {
      note.className = 'notice warn';
      note.textContent = `Expect ${human(p.native_seconds * WASM_SLOWDOWN)} in the browser (${human(p.native_seconds)} natively). The page stays usable; Stop ends the run.`;
    } else {
      note.textContent = 'This protocol verifies in well under a second.';
    }
  }

  function flushEditor() {
    if (!current || !currentFile) return;
    const ws = workspaces.get(current);
    if (ws.files[currentFile] !== editor.value) {
      ws.files[currentFile] = editor.value;
      ws.edited = !ws.custom;
      save();
    }
  }

  function addFiles(list) {
    const ws = workspaces.get(current);
    const reads = [...list].map(f => f.text().then(text => [f.name, text]));
    Promise.all(reads).then(pairs => {
      flushEditor();
      let target = ws;
      if (!ws.custom) target = makeCustom();
      for (const [name, text] of pairs) target.files[name] = text;
      const fpdl = pairs.find(([n]) => n.endsWith('.fpdl'));
      if (fpdl) target.entry = fpdl[0];
      save();
      select(target.name);
      if (fpdl) { currentFile = fpdl[0]; renderTabs(); }
    });
  }

  function makeCustom() {
    let i = 1;
    while (workspaces.has(`my-protocol-${i}`)) i++;
    const ws = { name: `my-protocol-${i}`, files: {}, entry: null, custom: true };
    workspaces.set(ws.name, ws);
    return ws;
  }

  function newFile() {
    flushEditor();
    const ws = workspaces.get(current);
    const hasFpdl = Object.keys(ws.files).some(n => n.endsWith('.fpdl'));
    let base = hasFpdl ? 'circuit' : 'protocol', ext = hasFpdl ? '.qasm' : '.fpdl', i = 1;
    let name = base + ext;
    while (ws.files[name] !== undefined) name = `${base}${++i}${ext}`;
    ws.files[name] = ext === '.fpdl' ? FPDL_TEMPLATE : QASM_TEMPLATE;
    if (!ws.custom) ws.edited = true;
    currentFile = name;
    save();
    renderTabs(); renderEntry();
  }

  function removeFile(name) {
    flushEditor();
    const ws = workspaces.get(current);
    delete ws.files[name];
    if (!ws.custom) ws.edited = true;
    if (currentFile === name) currentFile = Object.keys(ws.files)[0] || null;
    save();
    renderTabs(); renderEntry(); renderList();
  }

  function renameFile(tab, oldName) {
    const input = el('input', { type: 'text', value: oldName });
    input.setAttribute('aria-label', 'New file name');
    tab.replaceWith(input);
    input.focus(); input.select();
    let done = false;
    const finish = commit => {
      if (done) return; done = true;
      const ws = workspaces.get(current);
      const name = input.value.trim();
      if (commit && name && name !== oldName && ws.files[name] === undefined && !name.includes('/')) {
        flushEditor();
        ws.files[name] = ws.files[oldName];
        delete ws.files[oldName];
        if (ws.entry === oldName) ws.entry = name;
        if (currentFile === oldName) currentFile = name;
        if (!ws.custom) ws.edited = true;
        save();
      }
      renderTabs(); renderEntry();
    };
    input.addEventListener('keydown', e => { if (e.key === 'Enter') finish(true); if (e.key === 'Escape') finish(false); });
    input.addEventListener('blur', () => finish(true));
  }

  function resetWorkspace() {
    const p = bundled.find(b => b.name === current);
    if (!p) return;
    workspaces.set(p.name, { name: p.name, files: { ...p.files }, entry: p.entry, custom: false });
    save();
    select(p.name);
  }

  // --- running ---------------------------------------------------------------------
  function spawnWorker() {
    workerReady = false;
    worker = new Worker('worker.js');
    worker.onmessage = onWorkerMessage;
    worker.onerror = e => {
      line('error: ' + (e.message || 'the verifier failed to load'), 'err');
      finishRun('failed');
    };
  }

  function onWorkerMessage({ data }) {
    if (data.type === 'ready') {
      workerReady = true;
      if (statusEl.textContent.startsWith('Loading')) statusEl.textContent = 'Ready';
      updateRunButton();
    } else if (data.type === 'out') {
      let cls = data.err ? 'err' : '';
      if (/^\d+ unprotected path/.test(data.text)) cls = 'bad';
      else if (/^no unprotected path found/.test(data.text)) cls = 'good';
      line(data.text, cls);
    } else if (data.type === 'done') {
      line(`\n[exit ${data.rc}, ${(data.ms / 1000).toFixed(2)} s in the browser]`, 'sys');
      finishRun(data.rc === 0 ? 'Done' : 'Finished with errors');
    }
  }

  function line(text, cls) {
    const span = el('span', { textContent: text + '\n' });
    if (cls) span.className = cls;
    out.append(span);
    out.scrollTop = out.scrollHeight;
  }

  function updateRunButton() {
    const ws = current && workspaces.get(current);
    runBtn.disabled = running || !workerReady || !ws || !ws.entry;
  }

  function run() {
    flushEditor();
    const ws = workspaces.get(current);
    if (!ws || !ws.entry) return;
    const args = ['ftec-verify', '/w/' + ws.entry, '--fqcv=' + $('fqcv').value];
    if ($('first').checked) args.push('--first');
    if ($('dag').checked) args.push('--dag');
    if ($('bound').value) args.push('--bound=' + parseInt($('bound').value, 10));
    if ($('max-paths').value) args.push('--max-paths=' + parseInt($('max-paths').value, 10));

    out.textContent = '';
    line('$ ' + args.map(a => /[\s\[\]^]/.test(a) ? `"${a}"` : a).join(' ').replace('/w/', ''), 'sys');
    running = true;
    updateRunButton();
    stopBtn.disabled = false;
    const started = performance.now();
    statusEl.className = 'status running';
    const tick = () => { statusEl.textContent = `Running, ${((performance.now() - started) / 1000).toFixed(0)} s`; };
    tick();
    timer = setInterval(tick, 1000);
    worker.postMessage({ files: ws.files, args });
  }

  function finishRun(message) {
    clearInterval(timer);
    running = false;
    stopBtn.disabled = true;
    statusEl.className = 'status';
    statusEl.textContent = message;
    // Every run gets a fresh instance: start the next one now.
    if (worker) worker.terminate();
    spawnWorker();
    updateRunButton();
  }

  function stop() {
    if (!running) return;
    line('\n[stopped]', 'sys');
    finishRun('Stopped');
  }

  // --- templates -------------------------------------------------------------------
  // A new protocol starts as one unflagged round of the [[5,1,3]] code, which
  // a single fault already breaks: verify it to see what a failure looks like.
  const FPDL_TEMPLATE = `protocol my_protocol:

    code:
        [[5,1,3]]

        [[X,Z,Z,X,I],
         [I,X,Z,Z,X],
         [X,I,X,Z,Z],
         [Z,X,I,X,Z]]

    se:
        FSE_b {
            file: "./circuit.qasm"
            qd: data
            qm: syn
            g: [[X,Z,Z,X,I],
                [I,X,Z,Z,X],
                [X,I,X,Z,Z],
                [Z,X,I,X,Z]]
        }

    gvar:
        bit[4] s = ⊥;

    tc:
        1: (s != ⊥)

    af:
        while(!tc) {
            (s,) = FSE_b();
        }

    tp:
        1: { decode(mr[s]); }
`;
  const QASM_TEMPLATE = `OPENQASM 3.0;
include "stdgates.inc";

// One unflagged round of the [[5,1,3]] code: for each check, prepare the
// syndrome qubit in |+>, apply controlled Paulis onto the data, measure in X.

qubit[5] data;
qubit syn;
bit[4] m;

// g0 = XZZXI
reset syn; h syn;
cx syn,data[0]; cz syn,data[1]; cz syn,data[2]; cx syn,data[3];
h syn; m[0] = measure syn;

// g1 = IXZZX
reset syn; h syn;
cx syn,data[1]; cz syn,data[2]; cz syn,data[3]; cx syn,data[4];
h syn; m[1] = measure syn;

// g2 = XIXZZ
reset syn; h syn;
cx syn,data[0]; cx syn,data[2]; cz syn,data[3]; cz syn,data[4];
h syn; m[2] = measure syn;

// g3 = ZXIXZ
reset syn; h syn;
cz syn,data[0]; cx syn,data[1]; cx syn,data[3]; cz syn,data[4];
h syn; m[3] = measure syn;

`;

  // --- start -----------------------------------------------------------------------
  editor.addEventListener('input', () => { /* saved on blur/switch */ });
  editor.addEventListener('blur', flushEditor);
  editor.addEventListener('keydown', e => {
    if (e.key === 'Tab' && !e.shiftKey && !e.ctrlKey && !e.metaKey) {
      e.preventDefault();
      editor.setRangeText('    ', editor.selectionStart, editor.selectionEnd, 'end');
    }
    if ((e.ctrlKey || e.metaKey) && e.key === 'Enter') { e.preventDefault(); if (!runBtn.disabled) run(); }
  });
  for (const zone of [editor, tabs]) {
    zone.addEventListener('dragover', e => { e.preventDefault(); editor.classList.add('drop-hint'); });
    zone.addEventListener('dragleave', () => editor.classList.remove('drop-hint'));
    zone.addEventListener('drop', e => {
      e.preventDefault(); editor.classList.remove('drop-hint');
      if (e.dataTransfer.files.length) addFiles(e.dataTransfer.files);
    });
  }
  $('add-files').addEventListener('change', e => { addFiles(e.target.files); e.target.value = ''; });
  $('new-file').addEventListener('click', newFile);
  $('new-protocol').addEventListener('click', () => {
    flushEditor();
    const ws = makeCustom();
    ws.files = { 'protocol.fpdl': FPDL_TEMPLATE, 'circuit.qasm': QASM_TEMPLATE };
    ws.entry = 'protocol.fpdl';
    save();
    select(ws.name);
  });
  $('reset').addEventListener('click', resetWorkspace);
  entrySel.addEventListener('change', () => { workspaces.get(current).entry = entrySel.value; save(); });
  runBtn.addEventListener('click', run);
  stopBtn.addEventListener('click', stop);

  spawnWorker();

  fetch('protocols.json').then(r => r.json()).then(list => {
    bundled = list;
    for (const p of list) workspaces.set(p.name, { name: p.name, files: { ...p.files }, entry: p.entry, custom: false });
    const saved = load();
    for (const [name, ws] of Object.entries(saved)) {
      if (ws.custom) workspaces.set(name, { name, files: ws.files, entry: ws.entry, custom: true });
      else if (workspaces.has(name)) Object.assign(workspaces.get(name), { files: ws.files, entry: ws.entry, edited: true });
    }
    const wanted = new URLSearchParams(location.search).get('p');
    select(workspaces.has(wanted) ? wanted : 'LL25_[[5,1,3]]_[2,2]^T');
  }).catch(err => {
    plist.textContent = '';
    plist.append(el('p', { className: 'notice warn', textContent: 'Could not load the bundled protocols (' + err.message + '). You can still add your own files.' }));
    const ws = makeCustom();
    select(ws.name);
  });
})();

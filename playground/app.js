// Verifier playground: pick or write a protocol, edit its files, run the
// WebAssembly build of ftec-verify in a worker.

(() => {
  'use strict';

  // In the browser the verifier runs about 2.6 times slower than natively
  // (CB18 [[17,1,5]]: 121 s native, 310 s in WebAssembly).
  const WASM_SLOWDOWN = 2.6;
  const STORE_KEY = 'fpv-playground-v1';
  // The largest decision tree the playground draws, in tree nodes.
  const TREE_LIMIT = 1000;

  const $ = id => document.getElementById(id);
  const plist = $('plist'), tabs = $('tabs'), editor = $('editor'), entrySel = $('entry');
  const runBtn = $('run'), stopBtn = $('stop'), statusEl = $('status'), out = $('console');

  let bundled = [];                 // from protocols.json
  const workspaces = new Map();     // name -> { files, entry, custom, origin }
  let current = null;               // workspace name
  let currentFile = null;
  let worker = null, workerReady = false, running = false, timer = null;
  const treeView = $('tree-view');
  let treeRun = false, treeOut = [];
  const outTabs = $('out-tabs'), cexView = $('cex-view'), cexSelect = $('cex-select');
  let cex = null;                   // the last run's --json-out, with its list of counterexamples

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
  // The three verdicts of the results page's table run, worded as there:
  // circuit validity, protocol validity, correctability. Each is
  // [text, good]; a protocol without a run falls back to tests/expected.txt.
  function verdictsOf(p) {
    const v = p.verdicts;
    if (!v) { const e = verdictOf(p); return e ? [[e.text, e.good]] : []; }
    if (v.status && v.status !== 'ok' && v.failures === undefined) return [[v.status.replace(/\s*\(.*$/, ''), false]];
    const out = [];
    const plural = (n, word) => `${n} ${word}${n === 1 ? '' : 's'}`;
    if (v.circuit_valid !== undefined) {
      out.push(v.circuit_valid
        ? [v.circuit_warnings ? `circuits valid (${plural(v.circuit_warnings, 'sign warning')})` : 'circuits valid', true]
        : [`circuits: ${plural(v.circuit_problems, 'problem')}`, false]);
    }
    if (v.invalid_paths !== undefined && v.flag_bugs !== undefined) {
      const bugs = [];
      if (v.invalid_paths) bugs.push(plural(v.invalid_paths, 'control-flow bug path'));
      if (v.flag_bugs) bugs.push(plural(v.flag_bugs, 'flag bug'));
      out.push(bugs.length ? [`protocol not valid (${bugs.join(', ')})`, false] : ['protocol valid', true]);
    }
    if (v.failures !== undefined) {
      out.push(v.failures === 0 ? ['correctable', true]
        : [`not correctable (${plural(v.failures, 'path')} fail, t = ${v.min_t})`, false]);
    }
    return out;
  }
  function circuitsInvalid(p) { return !!(p.verdicts && p.verdicts.circuit_valid === false); }
  function outOfMemory(p) { return !!(p.verdicts && /memory/.test(p.verdicts.status || '')); }
  function verdictOf(p) {
    if (!p.expected) return null;
    const failures = /failures=(\d+)/.exec(p.expected);
    const minT = /min_t=(-?\d+)/.exec(p.expected);
    if (!failures) return null;
    const invalid = /invalid=(\d+)/.exec(p.expected);
    if (failures[1] !== '0') return { good: false, text: `not correctable at t = ${minT ? minT[1] : '?'}` };
    if (!invalid) return { good: true, text: 'correctable' };
    if (invalid[1] !== '0') return { good: false, text: 'control-flow bugs' };
    const flagBugs = /flagbugs=(\d+)/.exec(p.expected);
    if (flagBugs && flagBugs[1] !== '0') return { good: false, text: 'flag bugs' };
    return { good: true, text: 'fault tolerant' };
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
      for (const [text, good] of verdictsOf(p)) {
        meta.append(', ');
        meta.append(el('span', { className: good ? 'good' : 'bad', textContent: text }));
      }
      if (p.native_seconds === null) meta.append(outOfMemory(p) ? '' : p.verdicts ? ', did not finish natively' : ', not run with the current checks');
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
    if (circuitsInvalid(p)) {
      note.className = 'notice warn';
      note.textContent = 'These circuits are not valid, so the verifier stops after the circuit check. Run it to see why; tick “Skip the circuit check” to verify the circuits as written anyway (that is how the protocol validity and correctability listed for it were found).';
    } else if (p.native_seconds === null) {
      note.className = 'notice warn';
      note.textContent = outOfMemory(p)
        ? `This protocol ran out of memory natively (${p.verdicts.status}). It will run out of memory here too.`
        : p.verdicts ? 'This protocol did not finish natively. It will very likely run out of time or memory here.'
        : 'This protocol has not been run with the current checks. Natively it took about an hour, so it is very likely too large for the browser.';
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
      if (treeRun && !data.err) { treeOut.push(data.text); return; }
      let cls = data.err ? 'err' : '';
      if (/^\d+ unprotected path/.test(data.text) || /^(control-flow bugs|flag bugs\s*):\s*[1-9]/.test(data.text) || /^FTEC\s*: rejected/.test(data.text)) cls = 'bad';
      else if (/^no unprotected path found/.test(data.text) || /^(control-flow bugs|flag bugs\s*):\s*0 /.test(data.text) || /^FTEC\s*: accepted/.test(data.text)) cls = 'good';
      line(data.text, cls);
    } else if (data.type === 'done') {
      if (!treeRun && data.result) loadCounterexamples(data.result);
      if (treeRun && data.rc === 0 && showTree()) {
        finishRun('Done');
        return;
      }
      treeOut.forEach(t => line(t));
      line(`\n[exit ${data.rc}, ${(data.ms / 1000).toFixed(2)} s in the browser]`, 'sys');
      finishRun(data.rc === 0 ? 'Done' : 'Finished with errors');
    }
  }

  function showTree() {
    let data;
    try { data = JSON.parse(treeOut.join('\n')); } catch (_) { return false; }
    out.hidden = true;
    treeView.hidden = false;
    window.DecisionTree.render(treeView, data);
    return true;
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
    treeRun = $('tree').checked;
    treeOut = [];
    if (treeRun) args.push('--tree=' + TREE_LIMIT);
    else args.push('--json-out=/w/.result.json');
    if ($('skip-se').checked) args.push('--skip-circuit-check');
    if ($('no-input').checked) args.push('--no-input-errors');

    out.textContent = '';
    out.hidden = false;
    treeView.hidden = true;
    treeView.textContent = '';
    clearCounterexamples();
    line('$ ' + args.filter(a => !a.startsWith('--json-out')).map(a => /[\s\[\]^]/.test(a) ? `"${a}"` : a).join(' ').replace('/w/', ''), 'sys');
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

  // --- counterexamples ---------------------------------------------------------------
  // After a run, every failure the verifier traced back (--json-out) can be
  // picked from a list: its path is lit up in the decision tree, and the
  // executions behind it are spelled out, with their faults drawn on the
  // circuits. A flag bug shows its circuit instead.
  const esc = t => String(t).replace(/[&<>"]/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
  const subs = t => esc(t).replace(/\b([mf])(\d+)\b/g, '$1<sub>$2</sub>');
  const code = t => `<code>${esc(t)}</code>`;

  function clearCounterexamples() {
    cex = null;
    outTabs.hidden = true;
    cexView.hidden = true;
    $('cex-visual').textContent = '';
    $('cex-detail').textContent = '';
    cexSelect.textContent = '';
    setOutView('console');
  }

  function setOutView(view) {
    for (const b of outTabs.querySelectorAll('button')) b.setAttribute('aria-selected', String(b.dataset.view === view));
    out.hidden = view !== 'console';
    cexView.hidden = view !== 'cex';
    if (view === 'cex' && cex && !$('cex-visual').firstChild) showCounterexample(cexSelect.value);
  }
  outTabs.addEventListener('click', e => { const b = e.target.closest('button'); if (b) setOutView(b.dataset.view); });
  cexSelect.addEventListener('change', () => showCounterexample(cexSelect.value));

  function loadCounterexamples(text) {
    let data;
    try { data = JSON.parse(text); } catch (_) { return; }
    const items = [];
    (data.correctability || []).forEach((c, i) => items.push([`c${i}`, `Correctability · path ${c.path} · t = ${c.t}`]));
    (data.validity || []).forEach((v, i) => items.push([`v${i}`, `Control-flow bug · path ${v.path} (${v.action})`]));
    (data.flags || []).forEach((f, i) => items.push([`f${i}`, `Flag bug · ${f.se} is not ${f.k}-flag`]));
    if (!items.length) return;
    cex = data;
    for (const [value, label] of items) cexSelect.append(el('option', { value, textContent: label }));
    $('cex-count').textContent = `(${items.length})`;
    outTabs.hidden = false;
  }

  function highlightOf(item, witnesses) {
    const nodes = new Set(item.chain);
    const values = new Map(), marks = new Map();
    for (const e of item.record) values.set(e.node, `m${e.step}=${e.s}` + (e.f ? ` f${e.step}=${e.f}` : ''));
    for (const [w, label, cls] of witnesses) {
      for (const f of (w && w.faults) || []) {
        const e = item.record[f.step - 1];
        if (!e) continue;
        const list = marks.get(e.node) || [];
        if (!list.some(m => m.label === label)) list.push({ label, cls });
        marks.set(e.node, list);
      }
    }
    return { nodes, values, marks };
  }

  function showTreeFor(item, title, witnesses) {
    const box = el('div', { className: 'tree-view inner' });
    $('cex-visual').append(box);
    const tree = cex.tree && !cex.tree.too_large ? cex.tree : item.chain_tree;
    if (!tree) { box.textContent = 'No decision tree to show.'; return; }
    window.DecisionTree.render(box, tree, {
      title: title + (tree.chain ? ' · the whole tree is too large to draw, so only this path is shown' : ''),
      highlight: highlightOf(item, witnesses),
    });
  }

  function provenanceHtml(label, w, record, cls, single) {
    if (!w || !w.found) return `<div class="prov ${cls}"><h4>${esc(label)}</h4><p>Could not trace this error back.</p></div>`;
    const hasInput = /[XYZ]/.test(w.input_error);
    const items = [];
    items.push(hasInput ? `input error ${code(w.input_error)} (weight ${[...w.input_error].filter(c => c !== 'I').length})` : 'no input error');
    w.faults.forEach((f, i) => {
      const se = single || (record[f.step - 1] || {}).se || '?';
      const where = single ? '' : ` (circuit ${f.step}, outcomes m<sub>${f.step}</sub>)`;
      items.push(`fault ${i + 1}: ${code(f.pauli)} on ${esc(f.qubits.join(', '))} right after ${esc(f.gate)} #${f.instruction + 1} of ${esc(se)}${where}`);
    });
    if (!w.faults.length) items.push('no circuit fault');
    const stab = w.up_to_stabilizer ? ' (the error reported above times a stabilizer, which is the same error to the code)' : '';
    return `<div class="prov ${cls}"><h4>${esc(label)} = ${code(w.residual)} <span class="muted">on the data at the end${stab}</span></h4><ul>${items.map(i => `<li>${i}</li>`).join('')}</ul></div>`;
  }

  // One drawing per circuit run that holds a fault of any of the witnesses.
  function faultCircuits(detail, record, witnesses) {
    const bySteps = new Map();
    for (const [w, label, cls] of witnesses) {
      for (const f of (w && w.found && w.faults) || []) {
        const list = bySteps.get(f.step) || [];
        list.push({ instruction: f.instruction, pauli: f.pauli, qubits: f.qubits, cls, label, title: `${label}: ${f.pauli} on ${f.qubits.join(', ')}` });
        bySteps.set(f.step, list);
      }
    }
    for (const step of [...bySteps.keys()].sort((a, b) => a - b)) {
      const e = record[step - 1];
      const circuit = e && cex.circuits[e.se];
      if (!circuit) continue;
      const card = el('div', { className: 'tree-view circuit-card' });
      detail.append(card);
      window.CircuitView.render(card, circuit, { title: `Circuit ${step}: ${e.se} (${circuit.file}), m${step} = ${e.s}` + (e.f ? `, f${step} = ${e.f}` : ''), faults: bySteps.get(step) });
    }
  }

  function recordHtml(record) {
    return record.map(e => `m<sub>${e.step}</sub>=${esc(e.s)}` + (e.f ? ` f<sub>${e.step}</sub>=${esc(e.f)}` : '')).join(' · ');
  }

  function showCounterexample(value) {
    const visual = $('cex-visual'), detail = $('cex-detail');
    visual.textContent = '';
    detail.textContent = '';
    if (!cex || !value) return;
    const kind = value[0], item = { c: cex.correctability, v: cex.validity, f: cex.flags }[kind][+value.slice(1)];
    const html = [];

    if (kind === 'c') {
      const ws = [[item.witnesses[0], 'E1', 'w1'], [item.witnesses[1], 'E2', 'w2']];
      showTreeFor(item, `Path ${item.path}: the record below, outcome by outcome`, ws);
      html.push(`<h3>Correctability fails on path ${item.path} at t = ${item.t}</h3>`,
        `<p>Two executions with at most ${item.t} fault${item.t === 1 ? '' : 's'} (input error included) produce exactly this record, so the decoder sees the same thing, but the errors they leave differ by a logical operator: no correction fixes both. ${item.records > 1 ? `(${item.records} records of this path fail; this is the first.)` : ''}</p>`,
        `<p class="record">Record: ${recordHtml(item.record)}</p>`,
        `<p class="muted">${esc(item.detail)}</p>`,
        provenanceHtml('E1', item.witnesses[0], item.record, 'w1'),
        provenanceHtml('E2', item.witnesses[1], item.record, 'w2'));
      detail.innerHTML = html.join('');
      faultCircuits(detail, item.record, ws);
    } else if (kind === 'v') {
      const ws = [];
      (item.split || []).forEach((w, i) => ws.push([w, `C1-${i + 1}`, i ? 'w2' : 'w1']));
      if (item.heavy) ws.push([item.heavy, 'C3', 'w0']);
      showTreeFor(item, `Path ${item.path}: a record no condition settles`, ws);
      const rounds = (item.rounds || []).map((r, i) =>
        `<tr><td>round ${i + 1}</td><td>${r.steps.map(s => `m<sub>${s}</sub>`).join(' ')}</td><td>${r.flagged ? 'flag raised' : ''}</td><td>${r.counted ? `+${r.counted}` : '0'}</td></tr>`).join('');
      html.push(`<h3>Control-flow bug on path ${item.path} (${esc(item.action)})</h3>`,
        `<p>This record reaches the terminal, but it satisfies none of the three conditions of Def. 3.9, so the protocol cannot know it has seen enough to stop here.</p>`,
        `<p class="record">Record: ${recordHtml(item.record)}</p>`,
        `<div class="cond"><h4>Condition 1 fails</h4><p>${subs(item.why1)}</p>`,
        rounds ? `<p class="muted">Circuit faults this record proves: ${item.detected} (t = ${item.t}).</p><table class="rounds"><tr><th></th><th>outcomes</th><th></th><th>faults proved</th></tr>${rounds}</table>` : '',
        (item.split || []).map((w, i) => provenanceHtml(`Error ${i + 1} of level (0, t)`, w, item.record, i ? 'w2' : 'w1')).join(''),
        `</div><div class="cond"><h4>Condition 2 fails</h4><p>${subs(item.why2)}</p></div>`,
        `<div class="cond"><h4>Condition 3 fails</h4><p>${subs(item.why3)}</p>`,
        item.heavy ? provenanceHtml('The error', item.heavy, item.record, 'w0') : '', `</div>`);
      detail.innerHTML = html.join('');
      faultCircuits(detail, item.record, ws);
    } else {
      const circuit = cex.circuits[item.se];
      const w = item.witness;
      const box = el('div', { className: 'tree-view inner' });
      visual.append(box);
      if (circuit) {
        window.CircuitView.render(box, circuit, {
          title: `${item.se} (${circuit.file}) with the faults below`,
          faults: ((w && w.faults) || []).map((f, i) => ({ instruction: f.instruction, pauli: f.pauli, qubits: f.qubits, cls: 'w0', label: `fault ${i + 1}`, title: `${f.pauli} on ${f.qubits.join(', ')}` })),
        });
      }
      html.push(`<h3>Flag bug: ${esc(item.se)} is not ${item.k}-flag</h3>`,
        `<p>A record of path ${item.path} ends in repeated rounds (Condition 2), so the circuits of those rounds must satisfy the ${item.k}-flag condition (Def. 3.7): with no input error, any ${item.k} or fewer faults that raise no flag must leave an error of weight at most the number of faults, up to stabilizers. ${esc(item.se)} does not.</p>`,
        w && w.found ? `<p>The ${w.faults.length} fault${w.faults.length === 1 ? '' : 's'} drawn above raise no flag (every flag outcome reads 0), yet leave ${code(w.residual)} on the data, of minimum weight above ${w.s}.</p>` : `<p class="muted">${esc(item.problem)}</p>`,
        w && w.found ? provenanceHtml('The error', w, [], 'w0', item.se) : '',
        `<p class="muted">Needed by ${item.records} record${item.records === 1 ? '' : 's'} on ${item.paths} path${item.paths === 1 ? '' : 's'}, e.g. path ${item.path}: ${recordHtml(item.record)}</p>`);
      detail.innerHTML = html.join('');
    }
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

    ta:
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
    select(workspaces.has(wanted) ? wanted : 'CR17_[[5,1,3]]');
  }).catch(err => {
    plist.textContent = '';
    plist.append(el('p', { className: 'notice warn', textContent: 'Could not load the bundled protocols (' + err.message + '). You can still add your own files.' }));
    const ws = makeCustom();
    select(ws.name);
  });
})();

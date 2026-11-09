// SPBDD playground: edit a JavaScript program against the SPBDD bindings and
// run it in a worker.

(() => {
  'use strict';

  const SURFACE_3 = "['ZIIZIIIII', 'IZZIZZIII', 'IIIZZIZZI', 'IIIIIZIIZ',\n" +
                    "     'IXXIIIIII', 'XXIXXIIII', 'IIIIXXIXX', 'IIIIIIXXI']";

  const EXAMPLES = [
    ['Sets and products', `// A context fixes the number of qubits. Only one can be alive at a time.
const ctx = new spbdd.Context(2);

const s = ctx.from_list(['XI', 'ZZ']);
print('s          =', s);
print('s * s      =', s.mul(s));        // setwise product, phases ignored
print('|s * s|    =', s.mul(s).size());
print('s | {YY}   =', s.union(ctx.from_str('YY')));
print('s & {ZZ}   =', s.intersect(ctx.from_str('ZZ')));
print('all Paulis =', ctx.all().size());
print('<XI, ZZ>   =', ctx.generated_by(['XI', 'ZZ']));
`],
    ['Weight and patterns', `const ctx = new spbdd.Context(17);

// Sets far too large to list are still small diagrams.
for (const w of [1, 2, 3, 4]) {
  const e = ctx.weight_at_most(w);
  print(\`weight <= \${w}: \${e.size()} Paulis in \${e.node_count()} BDD nodes\`);
}

// '*' stands for any of I, X, Y, Z.
const p = ctx.matching('X*Z**************');
print('X on qubit 0 and Z on qubit 2:', p.size());
print('...and weight exactly 3:', p.intersect(ctx.weight_exactly(3)));
`],
    ['Clifford gates and measurement', `const ctx = new spbdd.Context(3);

// Gates act on every element at once, in place, and chain.
const e = ctx.from_list(['XII', 'IZI', 'IIY']);
print('before          :', e);
e.h(0).cx(0, 1).cz(1, 2);
print('after H0 CX01 CZ12:', e);

// Measuring Z on qubit 2 splits the set by whether each element
// commutes (meas_0) or anticommutes (meas_1) with Z2.
const m = e.measure_z(2);
print('commute with Z2    :', m.meas_0);
print('anticommute with Z2:', m.meas_1);
`],
    ['Faults through a circuit', `// Track every error one fault can leave behind in a small circuit:
// CX(0,1), then CX(1,2). Each two-qubit gate is a fault location.
const ctx = new spbdd.Context(3);

const t0 = ctx.identity();               // no faults: just III
const t1 = ctx.empty();                  // errors from exactly one fault

t0.cx(0, 1);
t1.cx(0, 1);
const spawned = t0.fault_inject([0, 1]); // a fault on the first gate
const t1a = t1.union(spawned);

t0.cx(1, 2);
t1a.cx(1, 2);
const t1b = t1a.union(t0.fault_inject([1, 2]));

print('one fault can leave', t1b.size(), 'different errors');
print('of weight 3:', t1b.intersect(ctx.weight_exactly(3)));
`],
    ['The [[5,1,3]] code', `const ctx = new spbdd.Context(5);
const code = new spbdd.Code(ctx, ['XZZXI', 'IXZZX', 'XIXZZ', 'ZXIXZ'], 3);

print('n, k, d  :', code.n(), code.k(), code.d());
print('logical X:', code.logical_X_operators(), ' logical Z:', code.logical_Z_operators());
print('|S| =', code.S().size(), ' |N(S)| =', code.N().size(), ' |N(S)-S| =', code.NmS().size());

for (const e of ['IIIII', 'XIIII', 'IZIII', 'IIYII'])
  print('syndrome of', e, '=', code.syndrome_of(e));

const w1 = ctx.weight_at_most(1);
print('weight <= 1 with syndrome 1000:', code.by_syndrome('1000').intersect(w1));
`],
    ['QEC condition, d = 3 surface code', `// A distance-3 code corrects every weight-1 error but not every weight-2 one.
// All four methods give the same answer; compare their cost.
const ctx = new spbdd.Context(9);
const code = new spbdd.Code(ctx, ${SURFACE_3}, 3);

for (const w of [1, 2]) {
  const errors = ctx.weight_at_most(w);
  for (const method of ['bruteforce', 'table', 'image', 'square']) {
    const t = performance.now();
    const v = code.find_qec_cond_violation(errors, method);
    const verdict = v ? \`violated by \${v[0]} and \${v[1]}\` : 'satisfied';
    print(\`weight <= \${w}, \${method.padEnd(10)} \${verdict}  (\${(performance.now() - t).toFixed(1)} ms)\`);
  }
}
`],
  ];

  const STORE_KEY = 'spbdd-playground-v1';
  const $ = id => document.getElementById(id);
  const editor = $('editor'), sel = $('example'), out = $('console');
  const runBtn = $('run'), stopBtn = $('stop'), statusEl = $('status');
  let worker = null, ready = false, running = false, timer = null;

  function stored() { try { return JSON.parse(localStorage.getItem(STORE_KEY) || '{}'); } catch (_) { return {}; } }
  function store(data) { try { localStorage.setItem(STORE_KEY, JSON.stringify(data)); } catch (_) { /* ignore */ } }

  EXAMPLES.forEach(([title], i) => sel.append(new Option(title, String(i))));
  const saved = stored();
  sel.value = String(saved.example ?? 0);
  editor.value = saved.code ?? EXAMPLES[Number(sel.value)][1];

  sel.addEventListener('change', () => {
    editor.value = EXAMPLES[Number(sel.value)][1];
    store({ example: Number(sel.value), code: editor.value });
  });
  $('reset').addEventListener('click', () => {
    editor.value = EXAMPLES[Number(sel.value)][1];
    store({ example: Number(sel.value), code: editor.value });
  });
  editor.addEventListener('input', () => store({ example: Number(sel.value), code: editor.value }));
  editor.addEventListener('keydown', e => {
    if (e.key === 'Tab' && !e.shiftKey && !e.ctrlKey && !e.metaKey) {
      e.preventDefault();
      editor.setRangeText('  ', editor.selectionStart, editor.selectionEnd, 'end');
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

  function spawn() {
    ready = false;
    worker = new Worker('worker.js');
    worker.onmessage = ({ data }) => {
      if (data.type === 'ready') { ready = true; if (statusEl.textContent.startsWith('Loading')) statusEl.textContent = 'Ready'; update(); }
      else if (data.type === 'out') line(data.text, data.err ? 'err' : '');
      else if (data.type === 'done') {
        line(`\n[${data.ok ? 'done' : 'failed'}, ${data.ms.toFixed(1)} ms]`, 'sys');
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

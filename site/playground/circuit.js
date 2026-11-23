// Draws one SE circuit (as ftec-verify --json-out lists it) with fault
// markers. Data qubits first, then syndrome and flag ancillas. Each operation
// takes the first column free on every wire it spans; a fault is drawn just
// after its gate, one letter per qubit, coloured by the error it belongs to.

window.CircuitView = (() => {
  'use strict';

  const DT = window.DecisionTree;
  const COL = 42, ROW = 34, TOP = 34, BOX = 22;
  const ROLE_ORDER = { data: 0, syndrome: 1, flag: 2, other: 3 };
  const GATE_TEXT = { h: 'H', s: 'S', sdg: 'S†', x: 'X', y: 'Y', z: 'Z', i: 'I', id: 'I' };

  function render(container, circuit, opts = {}) {
    container.textContent = '';
    const head = document.createElement('div');
    head.className = 'tree-head';
    const info = document.createElement('span');
    info.textContent = opts.title || circuit.file;
    const fit = document.createElement('button');
    fit.type = 'button'; fit.className = 'small-btn'; fit.textContent = 'Fit';
    head.append(info, fit);
    container.append(head);

    const svg = DT.svg;
    const regs = circuit.registers.slice().sort((a, b) => (ROLE_ORDER[a.role] ?? 3) - (ROLE_ORDER[b.role] ?? 3));
    const rows = [], rowOf = new Map();
    for (const r of regs) {
      for (let i = 0; i < r.width; i++) {
        const name = `${r.name}[${i}]`;
        rowOf.set(name, rows.length);
        rows.push({ name, role: r.role });
      }
    }
    const labelW = Math.max(...rows.map(r => DT.width(r.name, 12, '"IBM Plex Mono", monospace'))) + 22;
    const x0 = labelW + 18;
    const y = r => TOP + r * ROW;

    // Columns.
    const last = new Array(rows.length).fill(-1);
    const placed = [];
    for (const op of circuit.ops) {
      if (op.kind === 'barrier') {
        const c = Math.max(...last);
        last.fill(c);
        placed.push(null);
        continue;
      }
      const rs = op.qubits.map(q => rowOf.get(q)).filter(r => r !== undefined);
      if (!rs.length) { placed.push(null); continue; }
      const lo = Math.min(...rs), hi = Math.max(...rs);
      let c = -1;
      for (let r = lo; r <= hi; r++) c = Math.max(c, last[r]);
      c += 1;
      for (let r = lo; r <= hi; r++) last[r] = c;
      placed.push({ col: c, rows: rs });
    }
    const ncol = Math.max(0, ...last) + 1;
    const dims = { width: x0 + ncol * COL + 30, height: y(rows.length - 1) + ROW };

    const root = svg('svg', { class: 'tree-svg circuit-svg', role: 'img', 'aria-label': 'Circuit ' + circuit.file });
    const g = svg('g', {}, root);
    const wires = svg('g', { class: 'wires' }, g);
    rows.forEach((r, i) => {
      svg('line', { x1: labelW + 6, x2: dims.width - 12, y1: y(i), y2: y(i), class: 'wire' }, wires);
      svg('text', { x: labelW, y: y(i) + 4, 'text-anchor': 'end', class: 'qlabel ' + r.role }, wires).textContent = r.name;
    });

    const gates = svg('g', { class: 'gates' }, g);
    const box = (cx, cy, text, cls) => {
      svg('rect', { x: cx - BOX / 2, y: cy - BOX / 2, width: BOX, height: BOX, rx: 2, class: 'gbox ' + (cls || '') }, gates);
      svg('text', { x: cx, y: cy + 4, 'text-anchor': 'middle', class: 'gtext' }, gates).textContent = text;
    };
    circuit.ops.forEach((op, i) => {
      const p = placed[i];
      if (!p) return;
      const cx = x0 + p.col * COL;
      const [a, b] = p.rows;
      if (op.kind === 'measure') {
        box(cx, y(a), 'M', 'meas');
        svg('text', { x: cx, y: y(a) + BOX / 2 + 10, 'text-anchor': 'middle', class: 'gsub' }, gates).textContent = op.target;
      } else if (op.kind === 'reset') {
        box(cx, y(a), '0', 'reset');
      } else if (p.rows.length === 1) {
        box(cx, y(a), GATE_TEXT[op.gate] || op.gate);
      } else {
        svg('line', { x1: cx, x2: cx, y1: y(a), y2: y(b), class: 'gline' }, gates);
        svg('circle', { cx, cy: y(a), r: 4, class: 'dot' }, gates);
        if (op.gate === 'cx') {
          svg('circle', { cx, cy: y(b), r: 9, class: 'oplus' }, gates);
          svg('line', { x1: cx - 9, x2: cx + 9, y1: y(b), y2: y(b), class: 'gline' }, gates);
          svg('line', { x1: cx, x2: cx, y1: y(b) - 9, y2: y(b) + 9, class: 'gline' }, gates);
        } else if (op.gate === 'cz') {
          svg('circle', { cx, cy: y(b), r: 4, class: 'dot' }, gates);
        } else {
          box(cx, y(b), (op.gate || '').replace(/^c/, '').toUpperCase());
        }
      }
      svg('title', {}, gates.lastChild).textContent = `#${i + 1} ${op.kind === 'gate' ? op.gate : op.kind} ${op.qubits.join(', ')}`;
    });

    // Faults: after their gate, a letter on each qubit it acts on.
    const marks = svg('g', { class: 'faults' }, g);
    let focus = null;
    for (const f of opts.faults || []) {
      const p = placed[f.instruction];
      if (!p) continue;
      const cx = x0 + p.col * COL + COL / 2;
      const rs = f.qubits.map(q => rowOf.get(q));
      rs.forEach((r, j) => {
        if (r === undefined || f.pauli[j] === 'I') return;
        const m = svg('g', { class: 'fault ' + (f.cls || '') }, marks);
        svg('rect', { x: cx - 8, y: y(r) - 8, width: 16, height: 16, rx: 3 }, m);
        svg('text', { x: cx, y: y(r) + 4, 'text-anchor': 'middle' }, m).textContent = f.pauli[j];
        svg('title', {}, m).textContent = f.title || '';
      });
      const top = Math.min(...rs.filter(r => r !== undefined));
      if (f.label) {
        const t = svg('text', { x: cx, y: y(top) - 12, 'text-anchor': 'middle', class: 'fault-label ' + (f.cls || '') }, marks);
        t.textContent = f.label;
      }
      focus = focus ? { x: Math.min(focus.x, cx - 200), y: 0, w: Math.max(focus.x + focus.w, cx + 200) - Math.min(focus.x, cx - 200), h: dims.height }
                    : { x: cx - 200, y: 0, w: 400, h: dims.height };
    }

    container.append(root);
    const fitView = DT.panZoom(root, g, dims, focus);
    fit.addEventListener('click', fitView);
    requestAnimationFrame(fitView);
  }

  return { render };
})();

// Results page: rows are protocols, columns are (measure, run configuration)
// pairs the reader picks from the matrix. Data: results.json, written by
// tools/site_bench.py.
(() => {
  'use strict';

  const CONFIGS = [
    { id: 'table', label: 'table' },
    { id: 'image', label: 'image' },
    { id: 'square', label: 'square' },
    { id: 'bruteforce', label: 'bruteforce' },
  ];
  const DEFAULT_COLS = ['circuit_s@table', 'validity_s@table', 'correctability_s@table'];

  const num = v => (typeof v === 'number' && isFinite(v) ? v : null);
  const secs = v => v === null ? '' : v === 0 ? '0 s' : v < 1 ? Number(v.toPrecision(2)) + ' s' : v < 10 ? v.toFixed(2) + ' s' : v < 1000 ? v.toFixed(1) + ' s' : Math.round(v) + ' s';
  const bytes = v => {
    if (v === null) return '';
    if (v < 1024) return v + ' B';
    if (v < 1024 ** 2) return (v / 1024).toFixed(1) + ' KiB';
    if (v < 1024 ** 3) return (v / 1024 ** 2).toFixed(1) + ' MiB';
    return (v / 1024 ** 3).toFixed(2) + ' GiB';
  };
  const count = v => v === null ? '' : v.toLocaleString('en-US');
  const big = v => v === null ? '' : v >= 1e6 ? v.toExponential(2) : count(Math.round(v));

  // kind: 'time' | 'mem' | 'count' | 'verdict'. validity: needs the validity
  // check (not available in the no-validity run).
  const METRICS = [
    { group: 'Time', key: 'circuit_s', label: 'Circuit validity time', kind: 'time',
      get: r => num(r.circuit_s),
      def: 'Checking every extraction circuit once (signed Pauli propagation, no decision diagrams).' },
    { group: 'Time', key: 'validity_s', label: 'Protocol validity time', kind: 'time', validity: true,
      get: r => num(r.validity_s),
      def: 'Control-flow conditions 1–3 at every terminal record, including the flag condition of the final rounds.' },
    { group: 'Time', key: 'correctability_s', label: 'Correctability time', kind: 'time',
      get: r => num(r.traversal_s) === null ? null : Math.max(0, r.traversal_s - (r.validity_s || 0)),
      def: 'The traversal without the validity checks: state propagation, fault injection, measurement splitting and the final E1E2 query.' },
    { group: 'Time', key: 'fqcv_s', label: 'E1E2 query time', kind: 'time',
      get: r => num(r.fqcv_s),
      def: 'Only the final query of correctability (SPBDD’s find_qec_cond_violation), the part the four methods do differently.' },
    { group: 'Time', key: 'pre_fqcv_s', label: 'Propagation time', kind: 'time',
      get: r => num(r.pre_fqcv_s),
      def: 'Correctability time minus the E1E2 query: everything before the final query.' },
    { group: 'Time', key: 'validity_facts_s', label: 'Conditions 1 & 3 time', kind: 'time', validity: true,
      get: r => num(r.validity_facts_s), def: 'The semantic control-flow conditions, read off each terminal state.' },
    { group: 'Time', key: 'flag_condition_s', label: 'Flag condition time', kind: 'time', validity: true,
      get: r => num(r.flag_condition_s), def: 'Running circuits alone with up to k faults, for Condition 2.' },
    { group: 'Time', key: 'expansion_s', label: 'Expansion time', kind: 'time',
      get: r => num(r.expansion_s), def: 'Symbolic expansion of the FPDL control flow into paths.' },
    { group: 'Time', key: 'total_s', label: 'Total runtime', kind: 'time',
      get: r => num(r.total_s) ?? num(r.wall_s), def: 'Everything, as the verifier reports it.' },

    { group: 'Memory', key: 'peak_memory_bytes', label: 'Peak memory', kind: 'mem',
      get: r => num(r.peak_memory_bytes), def: 'Peak resident memory of the whole run.' },
    { group: 'Memory', key: 'circuit_peak_bytes', label: 'Peak after circuit check', kind: 'mem',
      get: r => num(r.circuit_peak_bytes), def: 'Peak resident memory when the circuit check has finished (before any decision diagram for correctability).' },
    { group: 'Memory', key: 'fqcv_rss_growth_bytes', label: 'E1E2 query memory', kind: 'mem',
      get: r => num(r.fqcv_rss_growth_bytes), def: 'The largest growth of resident memory during one E1E2 query.' },
    { group: 'Memory', key: 'validity_rss_growth_bytes', label: 'Validity memory', kind: 'mem', validity: true,
      get: r => num(r.validity_rss_growth_bytes), def: 'The largest growth of resident memory during one validity call.' },
    { group: 'Memory', key: 'diagram_peak', label: 'Peak BDD nodes', kind: 'count',
      get: r => num(r.diagram_peak), def: 'The largest number of live BuDDy nodes.' },

    { group: 'Verdict', key: 'ftec', label: 'FTEC', kind: 'verdict', validity: true,
      get: r => r.ftec ? (r.ftec === 'accepted' ? 1 : 0) : null,
      show: r => r.ftec ? [r.ftec, r.ftec === 'accepted' ? 'v-good' : 'v-bad'] : null,
      def: 'Accepted when circuits and control flow are valid and the protocol is correctable.' },
    { group: 'Verdict', key: 'circuit', label: 'Circuit validity', kind: 'verdict',
      get: r => r.circuit_valid === undefined ? null : (r.circuit_valid ? 1 : 0),
      show: r => r.circuit_valid === undefined ? null : r.circuit_valid
        ? [r.circuit_warnings ? `valid (${r.circuit_warnings} sign warning${r.circuit_warnings > 1 ? 's' : ''})` : 'valid', 'v-good']
        : [`${r.circuit_problems} problem${r.circuit_problems > 1 ? 's' : ''}`, 'v-bad'],
      def: 'Every extraction circuit is correct and deterministic. Invalid circuits are still verified as written (--skip-circuit-check) to fill the other columns.' },
    { group: 'Verdict', key: 'correctable', label: 'Correctable', kind: 'verdict',
      get: r => r.failures === undefined ? null : r.failures,
      show: r => r.failures === undefined ? null : r.failures === 0 ? ['yes', 'v-good'] : [`${r.failures} record${r.failures > 1 ? 's' : ''} fail, t = ${r.min_t}`, 'v-bad'],
      def: 'No terminal record leaves two errors with the same syndrome whose product is a non-trivial logical operator.' },
    { group: 'Verdict', key: 'control', label: 'Control flow', kind: 'verdict', validity: true,
      get: r => r.invalid === undefined ? null : r.invalid,
      show: r => r.invalid === undefined ? null : r.invalid === 0 ? ['valid', 'v-good'] : [`${count(r.invalid)} records, ${r.invalid_paths} path${r.invalid_paths > 1 ? 's' : ''}`, 'v-bad'],
      def: 'Every terminal record satisfies control-flow Condition 1, 2 or 3 (end() needs Condition 3).' },

    { group: 'Size', key: 'paths', label: 'Paths', kind: 'count', get: r => num(r.paths), def: 'Symbolic execution paths reached.' },
    { group: 'Size', key: 'records', label: 'Terminal records', kind: 'count', get: r => num(r.records), def: 'Distinct measurement records reaching a terminal.' },
    { group: 'Size', key: 'circuits', label: 'Circuits run', kind: 'count', get: r => num(r.circuits), def: 'Extraction circuits actually simulated.' },
    { group: 'Size', key: 'cond2', label: 'Records by Condition 2', kind: 'count', validity: true, get: r => num(r.cond2), def: 'Terminal records only Condition 2 settles.' },
    { group: 'Size', key: 'fqcv_setsize_max', label: 'Largest error set', kind: 'count', get: r => num(r.fqcv_setsize_max), def: 'Elements of the largest set given to the E1E2 query.', fmt: big },
  ];
  // Columns follow the order of the checks: circuit validity, then protocol
  // validity, then correctability, then whole-run measures.
  const STAGE = {
    circuit_s: 0, circuit_peak_bytes: 0, circuit: 0,
    validity_s: 1, validity_facts_s: 1, flag_condition_s: 1, validity_rss_growth_bytes: 1, control: 1, cond2: 1,
    ftec: 3, total_s: 3, peak_memory_bytes: 3,
  };
  const stageOf = key => STAGE[key] ?? 2;
  const GROUPS = [...new Set(METRICS.map(m => m.group))];
  METRICS.sort((a, b) => GROUPS.indexOf(a.group) - GROUPS.indexOf(b.group) || stageOf(a.key) - stageOf(b.key));
  const METRIC = Object.fromEntries(METRICS.map(m => [m.key, m]));
  const CONFIG = Object.fromEntries(CONFIGS.map(c => [c.id, c]));

  const $ = id => document.getElementById(id);
  const el = (tag, props, ...kids) => {
    const e = document.createElement(tag);
    Object.assign(e, props || {});
    for (const k of kids) if (k !== null && k !== undefined) e.append(k);
    return e;
  };

  let data = { runs: [], protocols: {} };
  let rows = [];           // [{name, n, k, d, note, runs: {config: run}}]
  let cols = [];           // ['metric@config']
  let sort = null;         // {col, dir}

  function readCols() {
    const m = /cols=([^&]*)/.exec(location.hash);
    if (m) return ordered(decodeURIComponent(m[1]).split(',').filter(valid));
    try { const s = localStorage.getItem('results-cols'); if (s) return ordered(s.split(',').filter(valid)); } catch (_) {}
    return DEFAULT_COLS.slice();
  }
  function ordered(list) {
    const rank = col => {
      const [k, c] = col.split('@');
      return [stageOf(k), METRICS.indexOf(METRIC[k]), CONFIGS.indexOf(CONFIG[c])];
    };
    return [...new Set(list)].sort((a, b) => {
      const x = rank(a), y = rank(b);
      return x[0] - y[0] || x[1] - y[1] || x[2] - y[2];
    });
  }
  function valid(col) {
    const [k, c] = col.split('@');
    return METRIC[k] && CONFIG[c] && !(METRIC[k].validity && CONFIG[c].noValidity);
  }
  function saveCols() {
    const value = cols.join(',');
    history.replaceState(null, '', '#cols=' + encodeURIComponent(value));
    try { localStorage.setItem('results-cols', value); } catch (_) {}
  }

  function buildRows() {
    const byName = new Map();
    const add = name => {
      if (!byName.has(name)) {
        const m = /\[\[\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\]\]/.exec(name);
        byName.set(name, { name, n: m ? +m[1] : 0, k: m ? +m[2] : 0, d: m ? +m[3] : 0, runs: {}, note: '' });
      }
      return byName.get(name);
    };
    for (const r of data.runs) add(r.protocol).runs[r.config] = r;
    for (const [name, note] of Object.entries(data.protocols || {})) add(name).note = note;
    rows = [...byName.values()].sort((a, b) => a.d - b.d || a.n - b.n || a.name.localeCompare(b.name));
  }

  // --- the column matrix ---------------------------------------------------
  function renderMatrix() {
    const t = $('matrix');
    t.textContent = '';
    const head = el('tr', {}, el('th', { scope: 'col', textContent: 'Measure' }));
    for (const c of CONFIGS) head.append(el('th', { scope: 'col', textContent: c.label }));
    t.append(el('thead', {}, head));
    const body = el('tbody');
    let group = null;
    for (const m of METRICS) {
      if (m.group !== group) {
        group = m.group;
        body.append(el('tr', { className: 'group' }, el('th', { colSpan: CONFIGS.length + 1, textContent: group })));
      }
      const tr = el('tr', {}, el('td', { textContent: m.label }));
      for (const c of CONFIGS) {
        const td = el('td');
        const col = `${m.key}@${c.id}`;
        if (m.validity && c.noValidity) {
          td.append(el('span', { className: 'na', textContent: '–', title: 'not measured without validity' }));
        } else {
          const box = el('input', { type: 'checkbox', checked: cols.includes(col) });
          box.setAttribute('aria-label', `${m.label}, ${c.label}`);
          box.addEventListener('change', () => {
            cols = box.checked ? ordered([...cols, col]) : cols.filter(x => x !== col);
            saveCols(); renderTable();
          });
          td.append(box);
        }
        tr.append(td);
      }
      body.append(tr);
    }
    t.append(body);
  }

  // --- the results table ---------------------------------------------------
  function cell(row, col) {
    const [k, c] = col.split('@');
    const m = METRIC[k];
    const run = row.runs[c];
    if (!run) return { text: row.note ? '' : '—', cls: 'v-none', title: row.note || 'not run yet', sort: null };
    const value = m.get(run);
    if (run.status && run.status.startsWith('timeout') && value === null) {
      return { text: 'timeout', cls: 'v-warn', title: run.status, sort: Infinity };
    }
    if (run.status && run.status !== 'ok' && value === null) {
      return { text: run.status, cls: 'v-bad', title: run.status, sort: null };
    }
    if (value === null) return { text: '—', cls: 'v-none', title: 'not reported', sort: null };
    if (m.show) {
      const [text, cls] = m.show(run) || ['—', 'v-none'];
      return { text, cls, title: run.circuits_skipped && k !== 'circuit' ? 'circuits verified as written (--skip-circuit-check)' : '', sort: value };
    }
    const fmt = m.fmt || (m.kind === 'time' ? secs : m.kind === 'mem' ? bytes : count);
    return { text: fmt(value), cls: '', title: '', sort: value };
  }

  function renderTable() {
    const t = $('results');
    t.textContent = '';
    const head = el('tr', {},
      el('th', { scope: 'col' }, sortButton('Protocol', '', null)),
      el('th', { scope: 'col' }, sortButton('Code', '', 'code')));
    for (const col of cols) {
      const [k, c] = col.split('@');
      const th = el('th', { scope: 'col', className: METRIC[k].kind === 'verdict' ? '' : 'num' });
      th.append(sortButton(METRIC[k].label, CONFIG[c].label, col));
      const rm = el('button', { className: 'remove', type: 'button', textContent: '×', title: 'Remove column' });
      rm.setAttribute('aria-label', `Remove ${METRIC[k].label}, ${CONFIG[c].label}`);
      rm.addEventListener('click', () => { cols = cols.filter(x => x !== col); saveCols(); renderMatrix(); renderTable(); });
      th.append(rm);
      head.append(th);
    }
    t.append(el('thead', {}, head));

    const body = el('tbody');
    let list = rows.slice();
    if (sort && sort.col) {
      const key = r => sort.col === 'code' ? r.n * 1000 + r.d : cell(r, sort.col).sort;
      list.sort((a, b) => {
        const x = key(a), y = key(b);
        if (x === null && y === null) return 0;
        if (x === null) return 1;
        if (y === null) return -1;
        return (x - y) * sort.dir;
      });
    }
    let dist = null;
    for (const r of list) {
      if (!sort && r.d !== dist) {
        dist = r.d;
        body.append(el('tr', { className: 'dist' }, el('th', { colSpan: cols.length + 2, textContent: `Distance ${r.d}` })));
      }
      const tr = el('tr', {}, el('td', { textContent: r.name.replace(/_/g, '_\u200b'), title: r.note || '' }), el('td', { className: 'code', textContent: `[[${r.n},${r.k},${r.d}]]` }));
      if (r.note && Object.keys(r.runs).length === 0) {
        tr.append(el('td', { colSpan: Math.max(cols.length, 1), className: 'v-none', textContent: r.note }));
      } else {
        for (const col of cols) {
          const c = cell(r, col);
          const [k] = col.split('@');
          const td = el('td', { className: [METRIC[k].kind === 'verdict' ? '' : 'num', c.cls].join(' ').trim(), textContent: c.text });
          if (c.title) td.title = c.title;
          tr.append(td);
        }
      }
      body.append(tr);
    }
    t.append(body);
    renderDefs();
    $('picker-count').textContent = `${cols.length} shown`;
  }

  function sortButton(label, sub, col) {
    const b = el('button', { type: 'button' });
    b.append(el('span', { textContent: label }));
    if (sub) b.append(el('span', { className: 'cfg', textContent: sub }));
    if (sort && sort.col === col) b.append(el('span', { className: 'arrow', textContent: sort.dir > 0 ? '▲ ascending' : '▼ descending' }));
    b.addEventListener('click', () => {
      if (col === null) sort = null;
      else if (sort && sort.col === col) sort = sort.dir > 0 ? { col, dir: -1 } : null;
      else sort = { col, dir: 1 };
      renderTable();
    });
    b.title = col === null ? 'Back to the default order' : 'Sort by this column';
    return b;
  }

  function renderDefs() {
    const dl = $('defs');
    dl.textContent = '';
    const seen = new Set();
    for (const col of cols) {
      const k = col.split('@')[0];
      if (seen.has(k)) continue;
      seen.add(k);
      dl.append(el('dt', { textContent: METRIC[k].label }), el('dd', { textContent: METRIC[k].def }));
    }
  }

  function csv() {
    const quote = s => /[",\n]/.test(s) ? `"${s.replace(/"/g, '""')}"` : s;
    const lines = [['protocol', 'code', ...cols.map(col => { const [k, c] = col.split('@'); return `${METRIC[k].label} (${CONFIG[c].label})`; })]];
    for (const r of rows) {
      lines.push([r.name, `[[${r.n},${r.k},${r.d}]]`, ...cols.map(col => {
        const [k, c] = col.split('@');
        const run = r.runs[c];
        if (!run) return '';
        const m = METRIC[k];
        const v = m.get(run);
        if (m.show) return v === null ? '' : (m.show(run) || [''])[0];
        return v === null ? (run.status || '') : String(v);
      })]);
    }
    const blob = new Blob([lines.map(l => l.map(quote).join(',')).join('\n') + '\n'], { type: 'text/csv' });
    const a = el('a', { href: URL.createObjectURL(blob), download: 'flag-protocol-verifier-results.csv' });
    document.body.append(a); a.click(); a.remove();
  }

  // The buttons sit inside the <summary>; keep their clicks from toggling it.
  for (const id of ['reset-cols', 'clear-cols', 'csv']) $(id).addEventListener('click', e => e.preventDefault());
  $('reset-cols').addEventListener('click', () => { cols = DEFAULT_COLS.slice(); sort = null; saveCols(); renderMatrix(); renderTable(); });
  $('clear-cols').addEventListener('click', () => { cols = []; saveCols(); renderMatrix(); renderTable(); });
  $('csv').addEventListener('click', csv);

  cols = readCols();
  renderMatrix();
  fetch('results.json', { cache: 'no-cache' })
    .then(r => { if (!r.ok) throw new Error(r.status); return r.json(); })
    .then(d => {
      data = d;
      buildRows();
      const configs = new Set(data.runs.map(r => r.config));
      const missing = CONFIGS.filter(c => !configs.has(c.id)).map(c => c.label);
      $('meta-line').textContent = `${rows.length} protocols. Updated ${data.updated || 'unknown'}. Verifier ${data.verifier || ''}.` +
        (missing.length ? ` Not measured yet: ${missing.join(', ')}.` : '');
      renderTable();
    })
    .catch(err => { $('meta-line').textContent = 'Could not load results.json (' + err.message + ').'; });
})();

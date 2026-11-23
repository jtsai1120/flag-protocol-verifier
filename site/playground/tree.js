// Draws the decision tree that `ftec-verify --tree` prints as JSON.
//
// Layout, after the usual drawing of a flag protocol: the adaptive rounds run
// down one column, and each branch that leaves them (a terminal action, a
// decoder) runs to the right, the first on the parent's own row. Chains of circuits that run unconditionally
// one after another in the same phase share one box.
// Of a node's af children, the one with the largest subtree continues down.
//
// Guards are shown as conjunctions, with measurement outcomes named m_k and
// f_k after the k-th circuit of the path (id_k.s and id_k.f in FPDL), and a
// guard that the others on its edge already imply is left out.

window.DecisionTree = (() => {
  'use strict';

  const SVG = 'http://www.w3.org/2000/svg';
  const FONT = 12, LINE = 17, PAD_X = 10, PAD_Y = 7, LABEL_FONT = 11, LABEL_LINE = 14;
  const ELBOW = 14, ROW_GAP = 26, COL_GAP_MIN = 46;

  // --- expressions -------------------------------------------------------------
  function tokenize(text) {
    const re = /\s*(==|!=|<=|>=|[()\[\]{},<>]|[A-Za-z_][A-Za-z_0-9]*(?:\.[A-Za-z_0-9]+)*|\d+)/y;
    const out = [];
    let m;
    while (re.lastIndex < text.length && (m = re.exec(text))) out.push(m[1]);
    if (re.lastIndex < text.trimEnd().length) throw new Error('cannot read ' + text);
    return out;
  }

  function parse(text) {
    const toks = tokenize(text);
    let i = 0;
    const peek = () => toks[i], next = () => toks[i++];
    const expect = t => { if (toks[i] !== t) throw new Error('expected ' + t); i++; };
    function or() {
      let a = and();
      while (peek() === 'or') { next(); a = { t: 'or', items: [a, and()] }; }
      return a;
    }
    function and() {
      let a = not();
      while (peek() === 'and') { next(); a = { t: 'and', items: [a, not()] }; }
      return a;
    }
    function not() {
      if (peek() === 'not') { next(); return { t: 'not', a: not() }; }
      const a = term();
      if (['==', '!=', '<', '>', '<=', '>='].includes(peek())) return { t: 'cmp', op: next(), a, b: term() };
      return a;
    }
    function list(close) {
      const items = [];
      if (peek() !== close) { items.push(or()); while (peek() === ',') { next(); items.push(or()); } }
      expect(close);
      return items;
    }
    function term() {
      const t = next();
      if (t === '(') { const e = or(); expect(')'); return e; }
      if (t === '{') return { t: 'tuple', items: list('}') };
      if (t === '[') return { t: 'list', items: list(']') };
      if (t === 'true' || t === 'false') return { t: 'bool', v: t === 'true' };
      if (/^\d+$/.test(t)) return { t: 'num', v: Number(t) };
      if (t === 'push' && peek() === '(') {
        next();
        const [base, item] = list(')');
        if (base.t === 'list') return { t: 'list', items: [...base.items, item] };
        return { t: 'push', base, item };
      }
      if (peek() === '[' && toks[i + 1] === ']') { i += 2; return { t: 'list', items: [] }; }
      if (t === undefined) throw new Error('unexpected end');
      return { t: 'var', name: t };
    }
    const e = or();
    if (i !== toks.length) throw new Error('trailing ' + toks[i]);
    return e;
  }

  const NEG = { '==': '!=', '!=': '==', '<': '>=', '>=': '<', '>': '<=', '<=': '>' };

  // Push negations down to the comparisons and drop constant parts.
  function normalize(e, negate = false) {
    switch (e.t) {
      case 'not': return normalize(e.a, !negate);
      case 'bool': return { t: 'bool', v: e.v !== negate };
      case 'cmp': return negate ? { ...e, op: NEG[e.op] } : e;
      case 'and': case 'or': {
        const t = (e.t === 'and') !== negate ? 'and' : 'or';
        const items = [];
        for (const item of e.items) {
          const n = normalize(item, negate);
          if (n.t === t) items.push(...n.items); else items.push(n);
        }
        const unit = t === 'and';          // and: drop true, false wins
        const kept = [];
        for (const n of items) {
          if (n.t === 'bool') { if (n.v === unit) continue; return { t: 'bool', v: !unit }; }
          if (!kept.some(k => show(k) === show(n))) kept.push(n);
        }
        if (!kept.length) return { t: 'bool', v: unit };
        return kept.length === 1 ? kept[0] : { t, items: kept };
      }
      default: return negate ? { t: 'not', a: e } : e;
    }
  }

  function varName(name) {
    const m = /^id_(\d+)\.([sf])$/.exec(name);
    return m ? (m[2] === 's' ? 'm' : 'f') + m[1] : name;
  }

  const OPS = { '==': '=', '!=': '≠', '<=': '≤', '>=': '≥', '<': '<', '>': '>' };
  function show(e, parent) {
    switch (e.t) {
      case 'var': return varName(e.name);
      case 'num': return String(e.v);
      case 'bool': return e.v ? 'true' : 'false';
      case 'list': return '[' + e.items.map(x => show(x)).join(', ') + ']';
      case 'tuple': return '(' + e.items.map(x => show(x)).join(' ') + ')';
      case 'push': return 'push(' + show(e.base) + ', ' + show(e.item) + ')';
      case 'not': return '¬(' + show(e.a) + ')';
      case 'cmp': return show(e.a, 'cmp') + ' ' + OPS[e.op] + ' ' + show(e.b, 'cmp');
      case 'and': case 'or': {
        const s = e.items.map(x => show(x, e.t)).join(e.t === 'and' ? ' ∧ ' : ' ∨ ');
        return parent && parent !== e.t ? '(' + s + ')' : s;
      }
    }
    return '?';
  }

  function collect(e, vars, consts) {
    if (e.t === 'var') vars.add(e.name);
    else if (e.t === 'num') consts.add(e.v);
    for (const k of ['a', 'b', 'base', 'item']) if (e[k]) collect(e[k], vars, consts);
    if (e.items) for (const x of e.items) collect(x, vars, consts);
  }

  function evaluate(e, env) {
    switch (e.t) {
      case 'var': return env[e.name];
      case 'num': case 'bool': return e.v;
      case 'list': case 'tuple': return e.items.map(x => evaluate(x, env));
      case 'not': return !evaluate(e.a, env);
      case 'and': return e.items.every(x => evaluate(x, env));
      case 'or': return e.items.some(x => evaluate(x, env));
      case 'cmp': {
        const a = evaluate(e.a, env), b = evaluate(e.b, env);
        if (e.op === '==' || e.op === '!=') return (JSON.stringify(a) === JSON.stringify(b)) === (e.op === '==');
        if (typeof a !== 'number' || typeof b !== 'number') throw new Error('order on non-numbers');
        return { '<': a < b, '>': a > b, '<=': a <= b, '>=': a >= b }[e.op];
      }
    }
    throw new Error('cannot evaluate');
  }

  // All values of the outcomes a set of formulas reads. An outcome of one
  // measured generator is 0 or 1; any other outcome ranges over the constants
  // it could be compared with, 0, 1, and one fresh value per such outcome (so
  // that equalities between them can go either way). Null when that is large.
  function assignments(formulas, measuresOf) {
    const vars = new Set(), consts = new Set([0, 1]);
    formulas.forEach(c => collect(c, vars, consts));
    const names = [...vars];
    const wide = names.filter(n => !(/\.f$/.test(n) || measuresOf(n) === 1));
    const fresh = wide.map((_, i) => 'v' + i);
    const domain = n => (/\.f$/.test(n) || measuresOf(n) === 1) ? [0, 1] : [...consts, ...fresh];
    let size = 1;
    for (const n of names) { size *= domain(n).length; if (size > 50000) return null; }
    const envs = [];
    const fill = (k, env) => {
      if (k === names.length) { envs.push({ ...env }); return; }
      for (const v of domain(names[k])) { env[names[k]] = v; fill(k + 1, env); }
    };
    fill(0, {});
    return { envs, names, consts: [...consts], domain };
  }

  // Leave out every conjunct the others imply, then, if a shorter formula of
  // one or two comparisons says the same over all those values, use it.
  function simplify(conjuncts, measuresOf) {
    const space = assignments(conjuncts, measuresOf);
    if (!space) return conjuncts;
    const { envs } = space;
    let kept = conjuncts.slice();
    try {
      if (kept.length > 1) {
        const order = kept.map((c, i) => i).sort((i, j) => show(kept[j]).length - show(kept[i]).length);
        const alive = new Set(kept.map((c, i) => i));
        for (const i of order) {
          const others = [...alive].filter(j => j !== i);
          if (!others.length) continue;
          if (envs.every(env => !others.every(j => evaluate(kept[j], env)) || evaluate(kept[i], env))) alive.delete(i);
        }
        kept = kept.filter((c, i) => alive.has(i));
      }
      const target = envs.map(env => kept.every(c => evaluate(c, env)));
      const current = kept.map(c => show(c, 'and')).join(' ∧ ');
      const atoms = [];
      for (const n of space.names) {
        for (const c of space.domain(n)) {
          if (typeof c !== 'number') continue;
          const v = { t: 'var', name: n }, k = { t: 'num', v: c };
          atoms.push({ t: 'cmp', op: '==', a: v, b: k }, { t: 'cmp', op: '!=', a: v, b: k });
        }
      }
      const same = f => envs.every((env, i) => evaluate(f, env) === target[i]);
      const candidates = [];
      for (const a of atoms) candidates.push(a);
      for (let i = 0; i < atoms.length && atoms.length < 60; i++) {
        for (let j = i + 1; j < atoms.length; j++) {
          if (atoms[i].a.name === atoms[j].a.name) continue;
          candidates.push({ t: 'and', items: [atoms[i], atoms[j]] }, { t: 'or', items: [atoms[i], atoms[j]] });
        }
      }
      for (const f of candidates) {
        if (show(f).length < current.length && same(f)) return f.t === 'and' ? f.items : [f];
      }
    } catch (_) { return conjuncts; }
    return kept;
  }

  function guardLines(guards, measuresOf = () => 0) {
    if (!guards.length) return [];
    let conjuncts = [];
    try {
      for (const g of guards) {
        const n = normalize(parse(g.expr), !g.expected);
        if (n.t === 'and') conjuncts.push(...n.items);
        else if (!(n.t === 'bool' && n.v)) conjuncts.push(n);
      }
      const seen = new Set();
      conjuncts = conjuncts.filter(c => { const s = show(c); if (seen.has(s)) return false; seen.add(s); return true; });
      conjuncts = simplify(conjuncts, measuresOf);
      if (conjuncts.length === 1) return [show(conjuncts[0])];
      // Pack the conjuncts into lines of a readable width.
      const lines = [];
      for (const c of conjuncts) {
        const s = show(c, 'and');
        const last = lines[lines.length - 1];
        if (last !== undefined && width(last + ' ∧ ' + s, LABEL_FONT, SERIF) <= 230) lines[lines.length - 1] = last + ' ∧ ' + s;
        else lines.push(lines.length ? '∧ ' + s : s);
      }
      return lines;
    } catch (_) {
      return guards.map((g, i) => (i ? '∧ ' : '') + (g.expected ? g.expr : 'not (' + g.expr + ')'));
    }
  }

  function recordText(record) {
    try { return show(parse(record)); } catch (_) { return record; }
  }

  // --- measuring and drawing -----------------------------------------------------
  const canvas = document.createElement('canvas').getContext('2d');
  function width(text, size, family) {
    canvas.font = `${size}px ${family}`;
    return canvas.measureText(text).width;
  }
  const MONO = '"IBM Plex Mono", ui-monospace, Menlo, monospace';
  const SERIF = '"Newsreader", Georgia, serif';

  function svg(tag, attrs, parent) {
    const node = document.createElementNS(SVG, tag);
    for (const [k, v] of Object.entries(attrs || {})) node.setAttribute(k, v);
    if (parent) parent.append(node);
    return node;
  }

  // Text with m3 / f12 drawn with subscripts.
  function richText(parent, text, attrs) {
    const t = svg('text', attrs, parent);
    let last = 0, shifted = false;
    const re = /\b([mf])(\d+)\b/g;
    let m;
    const plain = s => { if (!s) return; svg('tspan', shifted ? { dy: '-0.28em' } : {}, t).textContent = s; shifted = false; };
    while ((m = re.exec(text))) {
      plain(text.slice(last, m.index));
      svg('tspan', shifted ? { dy: '-0.28em' } : {}, t).textContent = m[1];
      svg('tspan', { dy: '0.28em', 'font-size': '75%' }, t).textContent = m[2];
      shifted = true;
      last = re.lastIndex;
    }
    plain(text.slice(last));
    if (shifted) svg('tspan', { dy: '-0.28em' }, t).textContent = '​';
    return t;
  }

  // --- the tree --------------------------------------------------------------
  function build(data, hl) {
    const byId = new Map(data.nodes.map(n => [n.id, n]));
    const kids = new Map(data.nodes.map(n => [n.id, []]));
    const edgeTo = new Map();
    for (const e of data.edges) { kids.get(e.from).push(e.to); edgeTo.set(e.to, e); }

    // Display nodes: a box absorbs the unconditional same-phase chain below it.
    const boxes = [];
    const onPath = [];                       // onPath[k]: the k-th circuit of the current path
    const measuresOf = name => {
      const m = /^id_(\d+)\.s$/.exec(name);
      return m && onPath[+m[1]] ? onPath[+m[1]].g.length : 0;
    };
    function make(id) {
      const n = byId.get(id);
      const box = { kind: n.kind, items: [n], children: [], guard: [] };
      if (n.kind === 'se') {
        box.phase = n.phase;
        let cur = id;
        for (;;) {
          const k = kids.get(cur);
          if (k.length !== 1) break;
          const c = byId.get(k[0]);
          if (c.kind !== 'se' || c.phase !== n.phase || edgeTo.get(c.id).guards.length) break;
          box.items.push(c);
          cur = c.id;
        }
        box.last = cur;
      } else {
        box.last = id;
      }
      boxes.push(box);
      for (const n of box.items) if (n.kind === 'se') onPath[n.step] = n;
      for (const k of kids.get(box.last)) {
        const guard = guardLines(edgeTo.get(k).guards, measuresOf);
        const child = make(k);
        child.guard = guard;
        box.children.push(child);
      }
      for (const n of box.items) if (n.kind === 'se') delete onPath[n.step];
      box.count = 1 + box.children.reduce((a, c) => a + c.count, 0);
      return box;
    }
    const root = make(data.nodes[0].id);
    if (hl) {
      for (const b of boxes) {
        b.hl = b.items.some(n => hl.nodes.has(n.id));
        b.marks = [];
        for (const n of b.items) for (const m of (hl.marks.get(n.id) || [])) b.marks.push(m);
      }
    }
    return { root, boxes, hl };
  }

  function sizeBox(box) {
    if (box.kind === 'start') {
      box.lines = ['Start'];
      box.w = 74; box.h = 34;
    } else if (box.kind === 'terminal') {
      const n = box.items[0];
      box.lines = [n.action === 'decode' ? 'decode' : n.action, 'path ' + n.path];
      box.w = Math.max(76, ...box.lines.map(l => width(l, FONT, SERIF) + 2 * PAD_X));
      box.h = 2 * LINE + 2 * PAD_Y;
      if (n.action === 'decode') { box.w = box.h = Math.max(box.w, 62); }
    } else {
      box.lines = box.items.map(n => n.name);
      box.vars = box.items.map(n => (box.values && box.values.get(n.id)) || 'm' + n.step + (n.flagged ? ' f' + n.step : ''));
      box.w = Math.max(...box.items.map((n, i) =>
        width(box.lines[i], FONT, MONO) + width(box.vars[i], LABEL_FONT, SERIF) + 14)) + 2 * PAD_X;
      box.h = box.items.length * LINE + 2 * PAD_Y;
    }
    box.labelW = box.guard.length ? Math.max(...box.guard.map(l => width(l, LABEL_FONT, SERIF))) : 0;
    box.labelH = box.guard.length * LABEL_LINE;
    box.children.forEach(sizeBox);
  }

  // Columns and rows. Returns the last row the subtree uses.
  function place(box, col, row, rows) {
    box.col = col; box.row = row;
    (rows[row] = rows[row] || []).push(box);
    const kids = box.children;
    // The adaptive flow continues down: the af child with the largest subtree.
    let down = null;
    if (box.kind !== 'start') {
      for (const k of kids) if (k.kind === 'se' && k.phase === 'af' && (!down || k.count >= down.count)) down = k;
    }
    let r = row, used = row;
    for (const k of kids) {
      k.parentRow = row;
      if (k === down) continue;
      k.dir = 'right';
      used = place(k, col + 1, r, rows);
      r = used + 1;
    }
    if (down) {
      down.dir = 'down';
      used = place(down, col, Math.max(r, row + 1), rows);
    }
    return used;
  }

  function layout(tree) {
    if (tree.hl) for (const b of tree.boxes) b.values = tree.hl.values;
    tree.root.children.forEach(sizeBox);
    sizeBox(tree.root);
    const rows = [];
    place(tree.root, 0, 0, rows);
    const boxes = tree.boxes;
    const ncol = Math.max(...boxes.map(b => b.col)) + 1;
    const colW = Array(ncol).fill(0);
    for (const b of boxes) colW[b.col] = Math.max(colW[b.col], b.w);
    const gap = Array(ncol).fill(COL_GAP_MIN);     // gap[c] lies left of column c
    for (const b of boxes) {
      if (b.dir === 'right') gap[b.col] = Math.max(gap[b.col], b.labelW + ELBOW + 24);
      if (b.dir === 'down') gap[b.col] = Math.max(gap[b.col], b.labelW - colW[b.col] / 2 + ELBOW + 30);
    }
    const colX = [];
    let x = 20;
    for (let c = 0; c < ncol; c++) { if (c) x += gap[c]; colX[c] = x + colW[c] / 2; x += colW[c]; }

    const rowY = [];
    let y = 20, prevH = 0;
    for (let r = 0; r < rows.length; r++) {
      const list = rows[r] || [];
      const h = Math.max(0, ...list.map(b => b.h));
      if (r === 0) y += h / 2;
      else {
        let step = prevH / 2 + h / 2 + ROW_GAP;
        for (const b of list) {
          if (b.dir === 'right' && b.row !== b.parentRow) step = Math.max(step, b.labelH + 14);
          if (b.dir === 'down') step = Math.max(step, prevH / 2 + h / 2 + b.labelH + 16);
        }
        y += step;
      }
      rowY[r] = y;
      prevH = h;
    }
    for (const b of boxes) { b.cx = colX[b.col]; b.cy = rowY[b.row]; }
    const width = x + 20;
    const height = y + (prevH / 2) + 20;
    return { width, height, colX, colW };
  }

  function draw(g, tree, dims) {
    const edges = svg('g', { class: 'edges' }, g);
    const labels = svg('g', { class: 'labels' }, g);
    const nodes = svg('g', { class: 'nodes' }, g);
    const colRight = c => dims.colX[c] + dims.colW[c] / 2;

    for (const b of tree.boxes) {
      for (const k of b.children) {
        let d, lx, ly, anchor;
        if (k.dir === 'down') {
          const y1 = b.cy + b.h / 2, y2 = k.cy - k.h / 2;
          d = `M${b.cx},${y1} V${y2}`;
          lx = b.cx - 8; anchor = 'end';
          ly = (y1 + y2) / 2 - (k.labelH / 2) + LABEL_LINE * 0.75;
        } else {
          const x0 = b.cx + b.w / 2, xe = colRight(b.col) + ELBOW, x2 = k.cx - k.w / 2;
          d = k.cy === b.cy ? `M${x0},${b.cy} H${x2}` : `M${x0},${b.cy} H${xe} V${k.cy} H${x2}`;
          lx = (k.cy === b.cy ? (x0 + x2) / 2 : (xe + x2) / 2); anchor = 'middle';
          ly = k.cy - 5 - k.labelH + LABEL_LINE * 0.8;
        }
        const on = tree.hl && b.hl && k.hl, off = tree.hl && !on;
        svg('path', { d, class: 'edge' + (on ? ' hl' : off ? ' dim' : ''), 'marker-end': `url(#${tree.arrow}${on ? '-hl' : ''})` }, edges);
        k.guard.forEach((line, i) => richText(labels, line, {
          x: lx, y: ly + i * LABEL_LINE, 'text-anchor': anchor, class: 'guard' + (on ? ' hl' : off ? ' dim' : ''),
        }));
      }
    }

    for (const b of tree.boxes) {
      const node = svg('g', { class: 'node n-' + (b.kind === 'se' ? b.phase : b.kind === 'terminal' ? b.items[0].action : b.kind) +
        (tree.hl ? (b.hl ? ' hl' : ' dim') : '') + (tree.hl && b.hl && b.kind === 'terminal' ? ' bad' : '') }, nodes);
      const x = b.cx - b.w / 2, y = b.cy - b.h / 2;
      if (b.kind === 'terminal' && b.items[0].action === 'decode') {
        svg('circle', { cx: b.cx, cy: b.cy, r: b.w / 2 }, node);
      } else {
        svg('rect', { x, y, width: b.w, height: b.h, rx: b.kind === 'se' ? 3 : 8 }, node);
      }
      if (b.kind === 'se') {
        b.lines.forEach((line, i) => {
          const ty = y + PAD_Y + LINE * i + LINE * 0.72;
          svg('text', { x: x + PAD_X, y: ty, class: 'se-name' }, node).textContent = line;
          richText(node, b.vars[i], { x: x + b.w - PAD_X, y: ty, 'text-anchor': 'end', class: 'se-vars' });
        });
      } else {
        const top = b.cy - (b.lines.length * LINE) / 2 + LINE * 0.72;
        b.lines.forEach((line, i) => {
          svg('text', { x: b.cx, y: top + LINE * i, 'text-anchor': 'middle', class: i ? 'term-sub' : 'term' }, node).textContent = line;
        });
      }
      svg('title', {}, node).textContent = tooltip(b);
      (b.marks || []).forEach((m, i) => {
        const mx = b.cx + b.w / 2 - 4 - i * 30, my = b.cy - b.h / 2 - 2;
        const badge = svg('g', { class: 'mark ' + m.cls }, node);
        svg('rect', { x: mx - 26, y: my - 9, width: 26, height: 14, rx: 7 }, badge);
        svg('text', { x: mx - 13, y: my + 2, 'text-anchor': 'middle' }, badge).textContent = m.label;
      });
    }
  }

  function tooltip(b) {
    if (b.kind === 'start') return 'Start';
    if (b.kind === 'terminal') {
      const n = b.items[0];
      let s = `path ${n.path}: ${n.action}`;
      if (n.condition !== undefined) s += `, terminal condition ${n.condition}`;
      if (n.record) s += `\nrecord: ${recordText(n.record)}`;
      return s;
    }
    return b.items.map(n => `${n.name}  (${n.phase}, round ${n.round}.${n.invocation}, outcomes m${n.step}` +
      (n.flagged ? `, f${n.step}` : '') + `)\n  measures ${n.g.join(', ')}`).join('\n');
  }

  // --- view: drag to move, wheel to zoom ------------------------------------------
  // Makes `root` (an svg) pan and zoom the group `g` holding a drawing of
  // dims.width x dims.height. Returns fit(), which shows all of it, or the
  // box `focus` ({x, y, w, h}) when the whole would be too small to read.
  function panZoom(root, g, dims, focus) {
    let k = 1, tx = 0, ty = 0;
    const apply = () => g.setAttribute('transform', `translate(${tx},${ty}) scale(${k})`);
    const fitTo = (x, y, w, h) => {
      const r = root.getBoundingClientRect();
      k = Math.min(1.4, (r.width - 24) / w, (r.height - 24) / h);
      if (!(k > 0)) k = 1;
      tx = (r.width - w * k) / 2 - x * k;
      ty = Math.max(10, (r.height - h * k) / 2) - y * k;
      apply();
    };
    const fit = () => {
      fitTo(0, 0, dims.width, dims.height);
      if (focus && k < 0.6) fitTo(focus.x, focus.y, focus.w, focus.h);
    };
    let drag = null;
    root.addEventListener('pointerdown', e => {
      if (e.button !== 0) return;
      drag = { x: e.clientX - tx, y: e.clientY - ty };
      root.setPointerCapture(e.pointerId);
      root.classList.add('dragging');
    });
    root.addEventListener('pointermove', e => {
      if (!drag) return;
      tx = e.clientX - drag.x; ty = e.clientY - drag.y;
      apply();
    });
    const end = () => { drag = null; root.classList.remove('dragging'); };
    root.addEventListener('pointerup', end);
    root.addEventListener('pointercancel', end);
    root.addEventListener('wheel', e => {
      e.preventDefault();
      const r = root.getBoundingClientRect();
      const px = e.clientX - r.left, py = e.clientY - r.top;
      const factor = Math.exp(-e.deltaY * (e.deltaMode === 1 ? 0.05 : 0.0015));
      const nk = Math.min(8, Math.max(0.05, k * factor));
      tx = px - (px - tx) * (nk / k);
      ty = py - (py - ty) * (nk / k);
      k = nk;
      apply();
    }, { passive: false });
    root.addEventListener('dblclick', fit);
    return fit;
  }

  let arrows = 0;

  // opts.highlight: { nodes: Set of node ids, values: Map id -> text shown in
  // place of the outcome names, marks: Map id -> [{label, cls}] }.
  function render(container, data, opts = {}) {
    container.textContent = '';
    const head = document.createElement('div');
    head.className = 'tree-head';
    const info = document.createElement('span');
    info.textContent = opts.title || `${data.protocol} · ${data.code}, t = ${data.tau} · ${data.paths} path${data.paths === 1 ? '' : 's'} · ${data.node_count} nodes`;
    const fit = document.createElement('button');
    fit.type = 'button'; fit.className = 'small-btn'; fit.textContent = 'Fit';
    head.append(info, fit);
    container.append(head);

    if (data.too_large) {
      const p = document.createElement('p');
      p.className = 'tree-too-large';
      p.textContent = `This decision tree has ${data.node_count} nodes, more than the playground draws (${data.limit}). It is too large to show.`;
      container.append(p);
      fit.remove();
      return;
    }

    const tree = build(data, opts.highlight);
    tree.arrow = 'dt-arrow-' + (++arrows);
    const dims = layout(tree);
    const root = svg('svg', { class: 'tree-svg', role: 'img', 'aria-label': 'Decision tree of ' + data.protocol });
    const defs = svg('defs', {}, root);
    for (const [id, cls] of [[tree.arrow, 'arrow'], [tree.arrow + '-hl', 'arrow hl']]) {
      const marker = svg('marker', { id, viewBox: '0 0 10 10', refX: 9, refY: 5, markerWidth: 7, markerHeight: 7, orient: 'auto-start-reverse' }, defs);
      svg('path', { d: 'M0,0 L10,5 L0,10 z', class: cls }, marker);
    }
    const g = svg('g', {}, root);
    draw(g, tree, dims);
    container.append(root);

    let focus = null;
    if (tree.hl) {
      const on = tree.boxes.filter(b => b.hl);
      if (on.length) {
        const x0 = Math.min(...on.map(b => b.cx - b.w / 2)) - 40, x1 = Math.max(...on.map(b => b.cx + b.w / 2)) + 40;
        const y0 = Math.min(...on.map(b => b.cy - b.h / 2)) - 40, y1 = Math.max(...on.map(b => b.cy + b.h / 2)) + 40;
        focus = { x: x0, y: y0, w: x1 - x0, h: y1 - y0 };
      }
    }
    const fitView = panZoom(root, g, dims, focus);
    fit.addEventListener('click', fitView);
    requestAnimationFrame(fitView);
  }

  return { render, guardLines, panZoom, svg, richText, width };
})();

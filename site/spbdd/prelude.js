// Makes the raw SPBDD bindings (web/spbdd_bind.cpp) pleasant from JavaScript:
// gates return the set so they chain, optional arguments may be left out,
// sets print as {XI, ZZ, ...}, and a C++ exception becomes a JS Error with the
// library's message. Shared by the playground worker and the Node smoke test.

function installSpbddPrelude(M) {
  const P = M.PSet.prototype;
  for (const g of ['x', 'y', 'z', 'h', 's', 'sdg', 'sx', 'sxdg', 'cx', 'cy', 'cz', 'swap', 'reset']) {
    const raw = P[g];
    P[g] = function (...args) { raw.apply(this, args); return this; };
  }
  for (const m of ['measure_z', 'measure_x', 'measure_y']) {
    const raw = P[m];
    P[m] = function (q, outcome) { return raw.call(this, q, outcome); };
  }
  const rawElements = P.elements;
  P.elements = function (limit = 1 << 20) { return rawElements.call(this, limit); };
  P.toString = function (limit = 32) {
    const els = rawElements.call(this, limit);
    const n = this.size();
    return '{' + els.join(', ') + (n > els.length ? `, … (${n} elements)` : '') + '}';
  };

  const C = M.Code.prototype;
  const rawFqcv = C.find_qec_cond_violation;
  C.find_qec_cond_violation = function (errors, method) { return rawFqcv.call(this, errors, method); };

  // new spbdd.Code(ctx, gens) or new spbdd.Code(ctx, gens, d)
  const RawCode = M.Code;
  function Code(ctx, gens, d) { return d === undefined ? new RawCode(ctx, gens) : new RawCode(ctx, gens, d); }
  Code.prototype = RawCode.prototype;

  const RawContext = M.Context;
  function Context(n, anc) { return anc === undefined ? new RawContext(n) : new RawContext(n, anc); }
  Context.prototype = RawContext.prototype;

  return { Context, Code, PSet: M.PSet };
}

function spbddErrorMessage(M, e) {
  if (typeof e === 'number') {
    try { return M.exception_message(e); } catch (_) { return 'C++ exception'; }
  }
  return e && e.message ? e.message : String(e);
}

function formatValue(v) {
  if (v === null || v === undefined) return String(v);
  if (typeof v === 'object' && typeof v.size === 'function' && typeof v.elements === 'function') return v.toString();
  if (Array.isArray(v)) return '[' + v.map(formatValue).join(', ') + ']';
  if (typeof v === 'object' && v.meas_0 && v.meas_1) return `{meas_0: ${v.meas_0}, meas_1: ${v.meas_1}}`;
  if (typeof v === 'object') { try { return JSON.stringify(v); } catch (_) { return String(v); } }
  return String(v);
}

if (typeof module !== 'undefined') module.exports = { installSpbddPrelude, spbddErrorMessage, formatValue };

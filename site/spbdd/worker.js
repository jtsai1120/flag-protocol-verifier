// Runs one SPBDD playground program in a fresh WebAssembly instance, so Stop
// can terminate it and nothing (contexts, sets) leaks into the next run.

importScripts('spbdd.js', 'prelude.js');

const post = (type, extra) => postMessage(Object.assign({ type }, extra));

const ready = createSpbdd({
  print: text => post('out', { text }),
  printErr: text => post('out', { text, err: true }),
}).then(M => { post('ready'); return M; });

onmessage = async event => {
  const M = await ready;
  const spbdd = installSpbddPrelude(M);
  const print = (...args) => post('out', { text: args.map(formatValue).join(' ') });
  const console = { log: print, error: (...a) => post('out', { text: a.map(formatValue).join(' '), err: true }) };
  const started = performance.now();
  let ok = true;
  try {
    const program = new Function('spbdd', 'print', 'console', '"use strict";\n' + event.data.code);
    await program(spbdd, print, console);
  } catch (e) {
    ok = false;
    const message = spbddErrorMessage(M, e);
    const where = e && e.stack ? (/<anonymous>:(\d+):(\d+)/.exec(e.stack) || null) : null;
    post('out', { text: 'error: ' + message + (where ? ` (line ${Number(where[1]) - 3})` : ''), err: true });
  }
  post('done', { ok, ms: performance.now() - started });
};

// Runs one SPBDD playground program in a fresh WebAssembly instance, so Stop
// can terminate it and nothing (contexts, sets) leaks into the next run.

importScripts('spbdd.js', 'cpp.js');

const post = (type, extra) => postMessage(Object.assign({ type }, extra));

const ready = createSpbdd({
  print: text => post('out', { text }),
  printErr: text => post('out', { text, err: true }),
}).then(M => { post('ready'); return M; });

onmessage = async event => {
  const M = await ready;
  const started = performance.now();
  let rc = 0, ok = true;
  try {
    rc = SpbddCpp.run(event.data.code, M, (text, err) => post('out', { text, err }));
  } catch (e) {
    ok = false;
    if (e instanceof SpbddCpp.CppError) post('out', { text: `error: line ${e.line}: ${e.message}`, err: true, line: e.line });
    else post('out', { text: 'error: ' + SpbddCpp.errorMessage(M, e), err: true });
  }
  post('done', { ok, rc, ms: performance.now() - started });
};

// Runs one verification. The page starts a fresh worker for every run, so a
// run can be stopped by terminating its worker, and BuDDy's global state never
// carries over from one run to the next.

importScripts('ftec.js');

const post = (type, extra) => postMessage(Object.assign({ type }, extra));

const ready = createFtec({
  print: text => post('out', { text }),
  printErr: text => post('out', { text, err: true }),
}).then(M => { post('ready'); return M; });

function callMain(M, args) {
  const ptrs = args.map(a => {
    const n = M.lengthBytesUTF8(a) + 1;
    const p = M._malloc(n);
    M.stringToUTF8(a, p, n);
    return p;
  });
  const argv = M._malloc(4 * (ptrs.length + 1));
  ptrs.forEach((p, i) => { M.HEAP32[(argv >> 2) + i] = p; });
  M.HEAP32[(argv >> 2) + ptrs.length] = 0;
  return M._ftec_main(ptrs.length, argv);
}

onmessage = async event => {
  const M = await ready;
  const { files, args } = event.data;
  try { M.FS.mkdir('/w'); } catch (_) { /* exists */ }
  for (const [name, text] of Object.entries(files)) M.FS.writeFile('/w/' + name, text);

  const started = performance.now();
  let rc;
  try {
    rc = callMain(M, args);
  } catch (error) {
    const message = String(error && error.message ? error.message : error);
    post('out', {
      text: /memory|abort/i.test(message)
        ? 'error: the verifier ran out of memory in the browser (' + message + ')'
        : 'error: ' + message,
      err: true,
    });
    rc = -1;
  }
  post('done', { rc, ms: performance.now() - started });
};

// Run a C++ program with the SPBDD playground's interpreter under Node, the
// way the browser does: node web/run_playground_cpp.js prog.cpp
// (Used by `make check-spbdd-playground` to compare against the same program built by g++.)
const path = require('path');
const site = path.join(__dirname, 'spbdd-playground');
const createSpbdd = require(path.join(site, 'spbdd.js'));
const cpp = require(path.join(site, 'cpp.js'));
const src = require('fs').readFileSync(process.argv[2], 'utf8');

createSpbdd().then(M => {
  try {
    process.exitCode = cpp.run(src, M, (line, err) => (err ? process.stderr : process.stdout).write(line + '\n'));
  } catch (e) {
    console.error(e instanceof cpp.CppError ? `error: line ${e.line}: ${e.message}` : 'error: ' + cpp.errorMessage(M, e));
    process.exitCode = 1;
  }
});

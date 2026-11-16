// A small C++ interpreter for the SPBDD playground.
//
// It runs a subset of C++17 against the WebAssembly build of SPBDD
// (web/spbdd_bind.cpp), so a playground program reads like, and in most cases
// is, a program you could compile with g++ against libspbdd.a:
//
//   #include "spbdd/spbdd.hpp"
//   #include <iostream>
//   using namespace spbdd;
//
//   int main() {
//       context ctx(2);
//       pset s = ctx.from_list({"XI", "ZZ"});
//       std::cout << (s * s).size() << std::endl;
//   }
//
// Supported: free functions (no overloading), global and local variables,
// `using` aliases, lambdas, if/else, for, range-for, while, do-while,
// break/continue/return, structured bindings of pairs and meas_result, and
// the usual operators. Types: bool, char, int (and the other integer types),
// double, std::string, std::vector, std::pair, std::optional, and SPBDD's
// context, pset, code and meas_result. Not supported: classes and structs,
// templates of your own, pointers, std::map and the rest of the library.
//
// One extension: `std::cout << s` prints a pset as {A, B, ...} (the first 32
// elements), and a std::vector as [a, b, ...]. Real C++ needs a loop for that.

(function (global) {
  'use strict';

  class CppError extends Error {
    constructor(message, line) { super(message); this.line = line; }
  }

  // ---------------------------------------------------------------------------
  // Tokens
  // ---------------------------------------------------------------------------

  const OPS = ['<<=', '>>=', '...', '::', '->', '++', '--', '<<', '>>', '<=', '>=', '==', '!=', '&&', '||',
               '+=', '-=', '*=', '/=', '%=', '&=', '|=', '^=',
               '{', '}', '(', ')', '[', ']', ';', ',', '.', '<', '>', '=', '+', '-', '*', '/', '%',
               '&', '|', '^', '~', '!', '?', ':'];

  function tokenize(src) {
    const toks = [];
    let i = 0, line = 1, lineStart = 0;
    const n = src.length;
    const err = msg => { throw new CppError(msg, line); };
    let atLineStart = true;
    while (i < n) {
      const c = src[i];
      if (c === '\n') { line++; i++; lineStart = i; atLineStart = true; continue; }
      if (c === ' ' || c === '\t' || c === '\r') { i++; continue; }
      if (c === '#' && atLineStart) {          // preprocessor: skipped (only #include is expected)
        const end = src.indexOf('\n', i);
        const text = src.slice(i, end < 0 ? n : end).trim();
        if (!/^#\s*(include|pragma)\b/.test(text)) err('only #include is supported, not ' + text.split(/\s+/)[0]);
        i = end < 0 ? n : end;
        continue;
      }
      atLineStart = false;
      if (c === '/' && src[i + 1] === '/') { while (i < n && src[i] !== '\n') i++; continue; }
      if (c === '/' && src[i + 1] === '*') {
        const end = src.indexOf('*/', i + 2);
        if (end < 0) err('unterminated comment');
        for (let k = i; k < end; k++) if (src[k] === '\n') line++;
        i = end + 2; continue;
      }
      const col = i - lineStart + 1;
      if (/[A-Za-z_]/.test(c)) {
        let j = i + 1;
        while (j < n && /[A-Za-z0-9_]/.test(src[j])) j++;
        toks.push({ k: 'id', v: src.slice(i, j), line, col });
        i = j; continue;
      }
      if (/[0-9]/.test(c) || (c === '.' && /[0-9]/.test(src[i + 1]))) {
        let m = /^(0[xX][0-9a-fA-F']+|0[bB][01']+|(?:[0-9][0-9']*)?(?:\.[0-9']*)?(?:[eE][+-]?[0-9]+)?)([uUlLzZfF]*)/.exec(src.slice(i));
        const body = m[1].replace(/'/g, ''), suffix = m[2];
        const isFloat = !/^0[xXbB]/.test(body) && /[.eE]/.test(body) || /[fF]/.test(suffix) && !/^0[xX]/.test(body);
        let v;
        if (/^0[bB]/.test(body)) v = parseInt(body.slice(2), 2);
        else if (/^0[xX]/.test(body)) v = parseInt(body.slice(2), 16);
        else v = Number(body);
        toks.push({ k: 'num', v, float: isFloat, line, col });
        i += m[0].length; continue;
      }
      if (c === '"' || c === "'") {
        let j = i + 1, s = '';
        while (j < n && src[j] !== c) {
          if (src[j] === '\n') err('unterminated literal');
          if (src[j] === '\\') {
            const e = src[j + 1];
            const map = { n: '\n', t: '\t', r: '\r', '0': '\0', '\\': '\\', '"': '"', "'": "'" };
            if (!(e in map)) err('unsupported escape \\' + e);
            s += map[e]; j += 2;
          } else s += src[j++];
        }
        if (j >= n) err('unterminated literal');
        if (c === "'" && s.length !== 1) err('a character literal holds one character');
        toks.push({ k: c === '"' ? 'str' : 'chr', v: s, line, col });
        i = j + 1; continue;
      }
      const op = OPS.find(o => src.startsWith(o, i));
      if (!op) err(`unexpected character '${c}'`);
      toks.push({ k: 'op', v: op, line, col });
      i += op.length;
    }
    toks.push({ k: 'eof', v: '', line, col: 1 });
    return toks;
  }

  // ---------------------------------------------------------------------------
  // Types
  // ---------------------------------------------------------------------------

  const INT_NAMES = new Set(['int', 'long', 'short', 'unsigned', 'signed', 'size_t', 'int64_t', 'int32_t',
                             'uint64_t', 'uint32_t', 'ptrdiff_t', 'uint8_t', 'int8_t']);
  const SIMPLE = {
    void: 'void', bool: 'bool', char: 'char', double: 'double', float: 'double', auto: 'auto',
    string: 'string', pset: 'pset', context: 'context', code: 'code', meas_result: 'meas',
    violation: 'violation', fqcv_method: 'fqcv_method',
  };
  const TEMPLATES = new Set(['vector', 'pair', 'optional', 'initializer_list', 'function']);

  const T = (name, args = []) => ({ name, args, ref: false, cnst: false });
  const typeName = t => !t ? '?' : t.name === 'vector' ? `std::vector<${typeName(t.args[0])}>`
    : t.name === 'pair' ? `std::pair<${typeName(t.args[0])}, ${typeName(t.args[1])}>`
    : t.name === 'optional' ? `std::optional<${typeName(t.args[0])}>`
    : t.name === 'string' ? 'std::string' : t.name;

  // ---------------------------------------------------------------------------
  // Parser
  // ---------------------------------------------------------------------------

  class Parser {
    constructor(toks) { this.t = toks; this.p = 0; this.aliases = new Map(); }
    get cur() { return this.t[this.p]; }
    peek(k = 1) { return this.t[Math.min(this.p + k, this.t.length - 1)]; }
    is(v) { const c = this.cur; return (c.k === 'op' || c.k === 'id') && c.v === v; }
    eat(v) { if (this.is(v)) { this.p++; return true; } return false; }
    expect(v, what) {
      if (this.is(v)) return this.t[this.p++];
      // '>>' closing two template argument lists at once
      if (v === '>' && this.is('>>')) { this.cur.v = '>'; return this.cur; }
      throw new CppError(`expected '${v}'${what ? ' ' + what : ''} but found '${this.cur.v || 'end of file'}'`, this.cur.line);
    }
    ident(what) {
      if (this.cur.k !== 'id') throw new CppError(`expected ${what || 'a name'} but found '${this.cur.v || 'end of file'}'`, this.cur.line);
      return this.t[this.p++].v;
    }

    // A qualified name: a::b::c, with leading std:: and spbdd:: dropped.
    qname() {
      const parts = [this.ident()];
      while (this.is('::') && this.peek().k === 'id') { this.p++; parts.push(this.ident()); }
      while (parts.length > 1 && (parts[0] === 'std' || parts[0] === 'spbdd')) parts.shift();
      return parts.join('::');
    }

    // Try to read a type at the current position; null (position restored) if there is none.
    tryType() {
      const save = this.p;
      let cnst = false, sawSpec = false;
      for (;;) {
        if (this.is('const') || this.is('constexpr')) { cnst = true; sawSpec = true; this.p++; }
        else if (this.is('static') || this.is('inline') || this.is('volatile')) { sawSpec = true; this.p++; }
        else break;
      }
      if (this.cur.k !== 'id') { this.p = save; return null; }
      let t = null;
      // integer spellings: unsigned long long, long int, ...
      if (INT_NAMES.has(this.cur.v) || (this.cur.v === 'std' && this.peek().v === '::' && INT_NAMES.has(this.peek(2).v))) {
        if (this.cur.v === 'std') this.p += 2;
        while (this.cur.k === 'id' && (INT_NAMES.has(this.cur.v) || this.cur.v === 'int')) this.p++;
        t = T('int');
      } else {
        const start = this.p;
        let name;
        try { name = this.qname(); } catch (e) { this.p = save; return null; }
        if (name === 'code::violation') name = 'violation';
        if (name === 'code::fqcv_method') name = 'fqcv_method';
        if (/^(map|set|unordered_map|unordered_set|array|deque|list|tuple|string_view|unique_ptr|shared_ptr)$/.test(name)) {
          throw new CppError(`std::${name} is not supported in the playground`, this.t[start].line);
        }
        if (this.aliases.has(name)) t = { ...this.aliases.get(name) };
        else if (SIMPLE[name]) t = T(SIMPLE[name]);
        else if (TEMPLATES.has(name) && this.is('<')) {
          this.p++;
          const args = [];
          if (name === 'function') {   // std::function<R(Args...)>: kept as an opaque callable
            let depth = 1;
            while (depth > 0 && this.cur.k !== 'eof') {
              if (this.is('<')) depth++;
              if (this.is('>')) depth--;
              if (this.is('>>')) depth -= 2;
              this.p++;
            }
            t = T('function');
          } else {
            do {
              const a = this.tryType();
              if (!a) { this.p = save; return null; }
              args.push(a);
            } while (this.eat(','));
            if (this.is('>>')) { this.cur.v = '>'; } else if (!this.eat('>')) { this.p = save; return null; }
            t = T(name === 'initializer_list' ? 'vector' : name, args);
          }
        } else { this.p = start; this.p = save; return null; }
      }
      if (this.eat('const')) cnst = true;
      if (this.is('*')) throw new CppError('pointers are not supported in the playground', this.cur.line);
      if (this.eat('&&') || this.eat('&')) t.ref = true;
      t.cnst = cnst;
      void sawSpec;
      return t;
    }

    // ---- top level ----
    program() {
      const funcs = new Map(), globals = [];
      while (this.cur.k !== 'eof') {
        if (this.eat(';')) continue;
        if (this.is('using')) { const s = this.usingDecl(); if (s) globals.push(s); continue; }
        if (this.is('struct') || this.is('class') || this.is('template') || this.is('namespace') || this.is('enum')) {
          throw new CppError(`'${this.cur.v}' is not supported in the playground`, this.cur.line);
        }
        const line = this.cur.line;
        const type = this.tryType();
        if (!type) throw new CppError(`expected a declaration but found '${this.cur.v}'`, line);
        if (this.cur.k === 'id' && this.peek().v === '(' && this.looksLikeFunction()) {
          const name = this.ident();
          const params = this.params();
          if (this.eat(';')) continue;            // a prototype
          const body = this.block();
          if (funcs.has(name) && funcs.get(name).body) throw new CppError(`'${name}' is defined twice (overloading is not supported)`, line);
          funcs.set(name, { k: 'func', name, ret: type, params, body, line });
          continue;
        }
        globals.push(this.declRest(type, line));
      }
      return { funcs, globals };
    }

    // After `type name (`: is it a function (parameters) or an object with constructor arguments?
    looksLikeFunction() {
      const save = this.p;
      this.p += 2;                                    // name (
      let fn = false;
      if (this.is(')')) fn = true;
      else if (this.is('void') && this.peek().v === ')') fn = true;
      else fn = !!this.tryType() && this.cur.k === 'id';
      this.p = save;
      return fn;
    }

    params() {
      this.expect('(');
      const ps = [];
      if (this.is('void') && this.peek().v === ')') this.p++;
      if (!this.is(')')) {
        do {
          const line = this.cur.line;
          const type = this.tryType();
          if (!type) throw new CppError('expected a parameter type', line);
          const name = this.cur.k === 'id' ? this.ident() : '_';
          if (this.eat('=')) throw new CppError('default arguments are not supported in the playground', line);
          ps.push({ type, name });
        } while (this.eat(','));
      }
      this.expect(')');
      if (this.eat('->')) this.tryType();
      return ps;
    }

    usingDecl() {
      const line = this.cur.line;
      this.expect('using');
      if (this.eat('namespace')) { this.qname(); this.expect(';'); return null; }
      if (this.cur.k === 'id' && this.peek().v === '=') {
        const name = this.ident(); this.p++;
        const type = this.tryType();
        if (!type) throw new CppError('expected a type after =', line);
        this.aliases.set(name, type);
        this.expect(';');
        return null;
      }
      this.qname(); this.expect(';');               // using std::cout;
      return null;
    }

    // ---- statements ----
    block() {
      const line = this.cur.line;
      this.expect('{');
      const body = [];
      while (!this.is('}')) {
        if (this.cur.k === 'eof') throw new CppError("missing '}'", line);
        const s = this.statement();
        if (s) body.push(s);
      }
      this.expect('}');
      return { k: 'block', body, line };
    }

    statement() {
      const line = this.cur.line;
      if (this.is('{')) return this.block();
      if (this.eat(';')) return null;
      if (this.is('using')) { this.usingDecl(); return null; }
      if (this.eat('if')) {
        this.eat('constexpr');
        this.expect('(');
        let init = null;
        const save = this.p;
        const t = this.tryType();
        let cond;
        if (t && this.cur.k === 'id') { init = this.declRest(t, line, true); cond = { k: 'var', name: init.decls[0].name, line }; }
        else { this.p = save; cond = this.expr(); }
        this.expect(')');
        const then = this.statement();
        const els = this.eat('else') ? this.statement() : null;
        return { k: 'if', init, cond, then, els, line };
      }
      if (this.eat('while')) {
        this.expect('('); const cond = this.expr(); this.expect(')');
        return { k: 'while', cond, body: this.statement(), line };
      }
      if (this.eat('do')) {
        const body = this.statement();
        this.expect('while'); this.expect('('); const cond = this.expr(); this.expect(')'); this.expect(';');
        return { k: 'do', cond, body, line };
      }
      if (this.eat('for')) {
        this.expect('(');
        let init = null;
        if (!this.is(';')) {
          const save = this.p;
          const t = this.tryType();
          if (t && (this.cur.k === 'id' || this.is('['))) {
            // range-for?
            if (this.is('[')) {
              const names = this.bindingNames();
              this.expect(':');
              const range = this.expr(); this.expect(')');
              return { k: 'rangefor', type: t, names, range, body: this.statement(), line };
            }
            if (this.peek().v === ':') {
              const name = this.ident(); this.p++;
              const range = this.expr(); this.expect(')');
              return { k: 'rangefor', type: t, name, range, body: this.statement(), line };
            }
            init = this.declRest(t, line, true);
          } else { this.p = save; init = { k: 'expr', e: this.expr(), line }; }
        }
        this.expect(';');
        const cond = this.is(';') ? null : this.expr();
        this.expect(';');
        const step = this.is(')') ? null : this.expr();
        this.expect(')');
        return { k: 'for', init, cond, step, body: this.statement(), line };
      }
      if (this.eat('return')) {
        const e = this.is(';') ? null : this.expr();
        this.expect(';');
        return { k: 'return', e, line };
      }
      if (this.eat('break')) { this.expect(';'); return { k: 'break', line }; }
      if (this.eat('continue')) { this.expect(';'); return { k: 'continue', line }; }
      if (this.is('struct') || this.is('class') || this.is('switch') || this.is('goto') || this.is('try') || this.is('throw')) {
        throw new CppError(`'${this.cur.v}' is not supported in the playground`, line);
      }
      // declaration or expression
      const save = this.p;
      const t = this.tryType();
      if (t && (this.cur.k === 'id' || this.is('['))) return this.declRest(t, line);
      this.p = save;
      const e = this.expr();
      this.expect(';', 'after the expression');
      return { k: 'expr', e, line };
    }

    bindingNames() {
      this.expect('[');
      const names = [];
      do names.push(this.ident()); while (this.eat(','));
      this.expect(']');
      return names;
    }

    // After the type: declarators up to ';' (or, inside if/for, without the ';').
    declRest(type, line, noSemi) {
      if (this.is('[')) {                                // structured binding
        const names = this.bindingNames();
        this.expect('=');
        const init = this.expr();
        if (!noSemi) this.expect(';');
        return { k: 'bind', type, names, init, line };
      }
      const decls = [];
      do {
        const name = this.ident('a variable name');
        if (this.is('[')) throw new CppError('C arrays are not supported; use std::vector', this.cur.line);
        let init = null, args = null;
        if (this.eat('=')) init = this.is('{') ? this.initList() : this.expr(true);
        else if (this.is('(')) args = this.args();
        else if (this.is('{')) args = this.initList().items, args.braced = true;
        decls.push({ name, init, args });
      } while (this.eat(','));
      if (!noSemi) this.expect(';', 'after the declaration');
      return { k: 'decl', type, decls, line };
    }

    args() {
      this.expect('(');
      const a = [];
      if (!this.is(')')) do a.push(this.expr(true)); while (this.eat(','));
      this.expect(')');
      return a;
    }

    initList() {
      const line = this.cur.line;
      this.expect('{');
      const items = [];
      if (!this.is('}')) {
        do { if (this.is('}')) break; items.push(this.expr(true)); } while (this.eat(','));
      }
      this.expect('}');
      return { k: 'list', items, line };
    }

    // ---- expressions (precedence climbing) ----
    // noComma: inside argument lists the comma operator is not allowed (it never is here).
    expr() { return this.assign(); }

    assign() {
      const left = this.ternary();
      const ops = ['=', '+=', '-=', '*=', '/=', '%=', '<<=', '>>=', '&=', '|=', '^='];
      if (this.cur.k === 'op' && ops.includes(this.cur.v)) {
        const op = this.cur.v, line = this.cur.line; this.p++;
        const right = this.is('{') ? this.initList() : this.assign();
        return { k: 'assign', op, left, right, line };
      }
      return left;
    }

    ternary() {
      const c = this.binary(0);
      if (this.is('?')) {
        const line = this.cur.line; this.p++;
        const a = this.assign(); this.expect(':'); const b = this.assign();
        return { k: 'cond', c, a, b, line };
      }
      return c;
    }

    binary(minPrec) {
      const PREC = { '||': 1, '&&': 2, '|': 3, '^': 4, '&': 5, '==': 6, '!=': 6, '<': 7, '<=': 7, '>': 7, '>=': 7,
                     '<<': 8, '>>': 8, '+': 9, '-': 9, '*': 10, '/': 10, '%': 10 };
      let left = this.unary();
      for (;;) {
        const c = this.cur;
        if (c.k !== 'op' || !(c.v in PREC) || PREC[c.v] <= minPrec - 1 || PREC[c.v] < minPrec) break;
        const op = c.v, prec = PREC[op];
        this.p++;
        const right = this.binary(prec + 1);
        left = { k: 'bin', op, a: left, b: right, line: c.line };
      }
      return left;
    }

    unary() {
      const c = this.cur;
      if (c.k === 'op' && ['-', '+', '!', '~', '++', '--', '*', '&'].includes(c.v)) {
        this.p++;
        const e = this.unary();
        if (c.v === '&') throw new CppError('taking addresses (&x) is not supported in the playground', c.line);
        return { k: 'unary', op: c.v, e, line: c.line };
      }
      if (c.k === 'id' && c.v === 'not') { this.p++; return { k: 'unary', op: '!', e: this.unary(), line: c.line }; }
      return this.postfix(this.primary());
    }

    postfix(e) {
      for (;;) {
        const c = this.cur;
        if (this.is('(')) { e = { k: 'call', f: e, args: this.args(), line: c.line }; continue; }
        if (this.is('[')) { this.p++; const i = this.expr(); this.expect(']'); e = { k: 'index', e, i, line: c.line }; continue; }
        if (this.is('.') || this.is('->')) {
          this.p++;
          const name = this.ident('a member name');
          e = { k: 'member', e, name, arrow: c.v === '->', line: c.line };
          continue;
        }
        if (this.is('++') || this.is('--')) { this.p++; e = { k: 'post', op: c.v, e, line: c.line }; continue; }
        return e;
      }
    }

    primary() {
      const c = this.cur, line = c.line;
      if (c.k === 'num') { this.p++; return { k: 'lit', v: c.float ? { t: 'double', v: c.v } : { t: 'int', v: c.v }, line }; }
      if (c.k === 'chr') { this.p++; return { k: 'lit', v: { t: 'char', v: c.v.charCodeAt(0) }, line }; }
      if (c.k === 'str') {
        let s = '';
        while (this.cur.k === 'str') s += this.t[this.p++].v;
        return { k: 'lit', v: { t: 'cstr', v: s }, line };
      }
      if (this.is('(')) { this.p++; const e = this.expr(); this.expect(')'); return e; }
      if (this.is('{')) return this.initList();
      if (this.is('[')) return this.lambda();
      if (c.k === 'id') {
        if (c.v === 'true' || c.v === 'false') { this.p++; return { k: 'lit', v: { t: 'bool', v: c.v === 'true' }, line }; }
        if (c.v === 'nullptr' || c.v === 'this' || c.v === 'new' || c.v === 'delete') throw new CppError(`'${c.v}' is not supported in the playground`, line);
        if (c.v === 'static_cast') {
          this.p++; this.expect('<'); const t = this.tryType(); this.expect('>'); this.expect('(');
          const e = this.expr(); this.expect(')');
          return { k: 'cast', type: t, e, line };
        }
        // a type used as a constructor: std::string(3, 'I'), pset(), std::vector<int>{1, 2}
        const save = this.p;
        const t = this.tryType();
        if (t && (this.is('(') || this.is('{')) && !['auto', 'void'].includes(t.name)) {
          const args = this.is('(') ? this.args() : this.initList().items;
          return { k: 'construct', type: t, args, line };
        }
        this.p = save;
        const name = this.qname();
        // std::max<int>(...): explicit template arguments on a function are ignored
        if (this.is('<') && ['max', 'min', 'move', 'make_pair'].includes(name)) { this.p++; this.tryType(); this.expect('>'); }
        return { k: 'var', name, line };
      }
      throw new CppError(`expected an expression but found '${c.v || 'end of file'}'`, line);
    }

    lambda() {
      const line = this.cur.line;
      this.expect('[');
      while (!this.is(']')) { if (this.cur.k === 'eof') throw new CppError("missing ']'", line); this.p++; }
      this.expect(']');
      const params = this.is('(') ? this.params() : [];
      this.eat('mutable');
      const body = this.block();
      return { k: 'lambda', params, body, line };
    }
  }

  // ---------------------------------------------------------------------------
  // Values
  //
  // A value is a tagged object: {t:'int'|'double'|'bool'|'char', v:number|bool},
  // {t:'string', v}, {t:'cstr', v} (a string literal), {t:'vector', et, items:[Cell]},
  // {t:'pair', first:Cell, second:Cell}, {t:'optional', v:value|null},
  // {t:'meas', meas_0:Cell, meas_1:Cell}, {t:'pset'|'context'|'code', o},
  // {t:'enum', v}, {t:'func', ...}, {t:'builtin', fn}, {t:'ostream'}, {t:'manip'},
  // {t:'void'}. A Cell holds a value: variables and vector elements are cells,
  // so references can share them.
  // ---------------------------------------------------------------------------

  class Cell {
    constructor(v) { this.val = v; }
    get() { return this.val; }
    set(v) { this.val = v; }
  }
  // s[i] for a std::string: writes go back to the string.
  class CharCell {
    constructor(owner, i, line) { this.owner = owner; this.i = i; this.line = line; }
    get() {
      const s = this.owner.get().v;
      if (this.i < 0 || this.i >= s.length) throw new CppError(`string index ${this.i} out of range (size ${s.length})`, this.line);
      return { t: 'char', v: s.charCodeAt(this.i) };
    }
    set(v) {
      const s = this.owner.get().v;
      if (this.i < 0 || this.i >= s.length) throw new CppError(`string index ${this.i} out of range (size ${s.length})`, this.line);
      this.owner.set({ t: 'string', v: s.slice(0, this.i) + String.fromCharCode(v.v) + s.slice(this.i + 1) });
    }
  }

  const VOID = { t: 'void' };
  const int = v => ({ t: 'int', v: Math.trunc(v) });
  const dbl = v => ({ t: 'double', v });
  const bool = v => ({ t: 'bool', v: !!v });
  const str = v => ({ t: 'string', v });
  const isNum = x => x.t === 'int' || x.t === 'double' || x.t === 'bool' || x.t === 'char';
  const isStr = x => x.t === 'string' || x.t === 'cstr';
  const desc = x => x.t === 'cstr' ? 'string literal' : x.t === 'vector' ? 'std::vector' : x.t === 'string' ? 'std::string' : x.t;

  // The C++ default stream format for double: like printf("%g") at the current precision.
  function formatDouble(x, prec, fixed) {
    if (!isFinite(x)) return isNaN(x) ? 'nan' : (x < 0 ? '-inf' : 'inf');
    if (fixed) return x.toFixed(prec);
    if (x === 0) return Object.is(x, -0) ? '-0' : '0';
    const p = prec === 0 ? 1 : prec;
    const exp = Math.floor(Math.log10(Math.abs(x)));
    let s;
    if (exp < -4 || exp >= p) {
      s = x.toExponential(p - 1);
      let [m, e] = s.split('e');
      if (m.includes('.')) m = m.replace(/0+$/, '').replace(/\.$/, '');
      const en = parseInt(e, 10);
      s = m + 'e' + (en < 0 ? '-' : '+') + String(Math.abs(en)).padStart(2, '0');
    } else {
      s = x.toFixed(Math.max(0, p - 1 - exp));
      if (s.includes('.')) s = s.replace(/0+$/, '').replace(/\.$/, '');
    }
    return s;
  }

  // ---------------------------------------------------------------------------
  // Interpreter
  // ---------------------------------------------------------------------------

  class Signal { constructor(kind, value) { this.kind = kind; this.value = value; } }

  class Env {
    constructor(parent) { this.parent = parent; this.vars = new Map(); this.owned = []; }
    lookup(name) {
      for (let e = this; e; e = e.parent) if (e.vars.has(name)) return e.vars.get(name);
      return null;
    }
    define(name, cell, owned, line) {
      if (this.vars.has(name)) throw new CppError(`'${name}' is already declared in this scope`, line);
      this.vars.set(name, cell);
      if (owned) this.owned.push(cell);
    }
  }

  function run(source, M, write) {
    const toks = tokenize(source);
    const parser = new Parser(toks);
    const prog = parser.program();

    const PSet = M.PSet, Context = M.Context, Code = M.Code;
    let callDepth = 0;
    let curLine = 0;
    const out = { buf: '', width: 0, prec: 6, fixed: false, boolalpha: false };

    const native = (line, fn) => {
      try { return fn(); } catch (e) {
        if (e instanceof CppError || e instanceof Signal) throw e;
        throw new CppError(errorMessage(M, e), line);
      }
    };

    // ---- conversions ----
    function num(x, line, what) {
      if (!isNum(x)) throw new CppError(`expected a number${what ? ' for ' + what : ''}, got ${desc(x)}`, line);
      return typeof x.v === 'boolean' ? (x.v ? 1 : 0) : x.v;
    }
    function truthy(x, line) {
      if (isNum(x)) return !!num(x);
      if (x.t === 'optional') return x.v !== null;
      throw new CppError(`${desc(x)} cannot be used as a condition`, line);
    }
    function jsString(x, line, what) {
      if (isStr(x)) return x.v;
      if (x.t === 'char') return String.fromCharCode(x.v);
      throw new CppError(`expected a string${what ? ' for ' + what : ''}, got ${desc(x)}`, line);
    }
    function jsInts(x, line, what) {
      if (x.t !== 'vector' && x.t !== 'list') throw new CppError(`expected a list of qubits${what ? ' for ' + what : ''}, got ${desc(x)}`, line);
      return x.items.map(c => num(c.get(), line, what));
    }
    function jsStrings(x, line, what) {
      if (x.t !== 'vector' && x.t !== 'list') throw new CppError(`expected a list of Pauli strings${what ? ' for ' + what : ''}, got ${desc(x)}`, line);
      return x.items.map(c => jsString(c.get(), line, what));
    }
    function bits(x, line) {
      if (isStr(x)) return x.v;
      if (x.t === 'vector' || x.t === 'list') return x.items.map(c => (num(c.get(), line) ? '1' : '0')).join('');
      throw new CppError(`expected a std::vector<bool>, got ${desc(x)}`, line);
    }
    const boolVector = s => ({ t: 'vector', et: T('bool'), items: [...s].map(ch => new Cell(bool(ch === '1'))) });
    const strVector = arr => ({ t: 'vector', et: T('string'), items: arr.map(s => new Cell(str(s))) });
    const pset = o => ({ t: 'pset', o });

    // A deep copy, as C++ value semantics require.
    function copy(x) {
      switch (x.t) {
        case 'pset': return pset(x.o.clone());
        case 'vector': return { t: 'vector', et: x.et, items: x.items.map(c => new Cell(copy(c.get()))) };
        case 'pair': return { t: 'pair', first: new Cell(copy(x.first.get())), second: new Cell(copy(x.second.get())) };
        case 'meas': return { t: 'meas', meas_0: new Cell(copy(x.meas_0.get())), meas_1: new Cell(copy(x.meas_1.get())) };
        case 'optional': return { t: 'optional', v: x.v === null ? null : copy(x.v) };
        case 'cstr': return str(x.v);
        case 'context': case 'code': return x;
        default: return x;
      }
    }

    // Convert a value to a declared type (copying it).
    function convert(x, type, line) {
      if (!type || type.name === 'auto') {
        if (x.t === 'list') return { t: 'vector', et: null, items: x.items.map(c => new Cell(copy(c.get()))) };
        if (x.t === 'void') throw new CppError('cannot store the result of a void function', line);
        return copy(x);
      }
      const n = type.name;
      switch (n) {
        case 'int': if (x.t === 'cstr' || x.t === 'string') break; return int(num(x, line));
        case 'double': return dbl(num(x, line));
        case 'bool': if (x.t === 'optional') return bool(x.v !== null); return bool(num(x, line));
        case 'char': return { t: 'char', v: num(x, line) & 255 };
        case 'string':
          if (isStr(x)) return str(x.v);
          break;
        case 'vector': {
          if (x.t === 'vector' || x.t === 'list') {
            const et = type.args[0];
            return { t: 'vector', et, items: x.items.map(c => new Cell(convert(c.get(), et, line))) };
          }
          break;
        }
        case 'pair': {
          if (x.t === 'pair') return { t: 'pair', first: new Cell(convert(x.first.get(), type.args[0], line)), second: new Cell(convert(x.second.get(), type.args[1], line)) };
          if (x.t === 'list' && x.items.length === 2) return { t: 'pair', first: new Cell(convert(x.items[0].get(), type.args[0], line)), second: new Cell(convert(x.items[1].get(), type.args[1], line)) };
          break;
        }
        case 'optional': case 'violation': {
          if (x.t === 'optional') return copy(x);
          if (x.t === 'nullopt') return { t: 'optional', v: null };
          const inner = n === 'violation' ? T('pair', [T('string'), T('string')]) : type.args[0];
          return { t: 'optional', v: convert(x, inner, line) };
        }
        case 'pset': if (x.t === 'pset') return copy(x); break;
        case 'meas': if (x.t === 'meas') return copy(x); break;
        case 'context': case 'code': if (x.t === n) return x; break;
        case 'fqcv_method': if (x.t === 'enum') return x; break;
        case 'function': if (x.t === 'func' || x.t === 'builtin') return x; break;
        default: break;
      }
      if (x.t === n) return copy(x);
      throw new CppError(`cannot convert ${desc(x)} to ${typeName(type)}`, line);
    }

    function release(x) {
      // Free BDD references held by a value going out of scope.
      try {
        if (x.t === 'pset' && x.o) x.o.delete();
        else if (x.t === 'vector') x.items.forEach(c => release(c.get()));
        else if (x.t === 'pair') { release(x.first.get()); release(x.second.get()); }
        else if (x.t === 'meas') { release(x.meas_0.get()); release(x.meas_1.get()); }
        else if (x.t === 'optional' && x.v) release(x.v);
      } catch (_) { /* already gone */ }
    }
    function leave(env) {
      for (let i = env.owned.length - 1; i >= 0; i--) {
        const v = env.owned[i].get();
        if (v.t === 'context') {
          // only the variable that constructed the context destroys it
          if (v.owner === env.owned[i] && !v.dead) { v.dead = true; try { v.o.delete(); } catch (_) { /* */ } }
        } else if (v.t !== 'code') release(v);
      }
    }

    // ---- globals: std:: and spbdd:: names; `top` holds the program's own globals ----
    const globals = new Env(null);
    const top = new Env(globals);
    const builtin = (name, fn) => globals.define(name, new Cell({ t: 'builtin', name, fn }));
    globals.define('cout', new Cell({ t: 'ostream', err: false }));
    globals.define('cerr', new Cell({ t: 'ostream', err: true }));
    globals.define('endl', new Cell({ t: 'manip', name: 'endl' }));
    globals.define('flush', new Cell({ t: 'manip', name: 'flush' }));
    globals.define('boolalpha', new Cell({ t: 'manip', name: 'boolalpha' }));
    globals.define('noboolalpha', new Cell({ t: 'manip', name: 'noboolalpha' }));
    globals.define('fixed', new Cell({ t: 'manip', name: 'fixed' }));
    globals.define('defaultfloat', new Cell({ t: 'manip', name: 'defaultfloat' }));
    globals.define('nullopt', new Cell({ t: 'nullopt' }));
    globals.define('string::npos', new Cell(int(-1)));
    for (const m of ['bruteforce', 'table', 'image', 'square']) {
      globals.define('code::fqcv_method::' + m, new Cell({ t: 'enum', v: m }));
      globals.define('fqcv_method::' + m, new Cell({ t: 'enum', v: m }));
    }
    builtin('setw', (a, line) => ({ t: 'manip', name: 'setw', n: num(a[0], line) }));
    builtin('setprecision', (a, line) => ({ t: 'manip', name: 'setprecision', n: num(a[0], line) }));
    builtin('to_string', (a, line) => {
      const x = a[0];
      if (x.t === 'double') return str(x.v.toFixed(6));
      return str(String(num(x, line)));
    });
    builtin('max', (a, line) => (num(a[0], line) >= num(a[1], line) ? a[0] : a[1]));
    builtin('min', (a, line) => (num(a[0], line) <= num(a[1], line) ? a[0] : a[1]));
    builtin('abs', (a, line) => (a[0].t === 'double' ? dbl(Math.abs(a[0].v)) : int(Math.abs(num(a[0], line)))));
    builtin('pow', (a, line) => dbl(Math.pow(num(a[0], line), num(a[1], line))));
    builtin('sqrt', (a, line) => dbl(Math.sqrt(num(a[0], line))));
    builtin('log2', (a, line) => dbl(Math.log2(num(a[0], line))));
    builtin('move', a => a[0]);
    builtin('make_pair', a => ({ t: 'pair', first: new Cell(copy(a[0])), second: new Cell(copy(a[1])) }));
    builtin('context::alive', () => bool(Context.alive()));

    // ---- output ----
    function emit(text, err) {
      out.buf += text;
      let i;
      while ((i = out.buf.indexOf('\n')) >= 0) { write(out.buf.slice(0, i), err); out.buf = out.buf.slice(i + 1); }
    }
    function show(x, line) {
      switch (x.t) {
        case 'int': return String(x.v);
        case 'double': return formatDouble(x.v, out.prec, out.fixed);
        case 'bool': return out.boolalpha ? String(x.v) : (x.v ? '1' : '0');
        case 'char': return String.fromCharCode(x.v);
        case 'string': case 'cstr': return x.v;
        case 'pset': {
          const els = native(line, () => x.o.elements(32));
          const n = native(line, () => x.o.size());
          return '{' + els.join(', ') + (n > els.length ? `, ... (${formatDouble(n, 6, false)} elements)` : '') + '}';
        }
        case 'vector': case 'list': return '[' + x.items.map(c => show(c.get(), line)).join(', ') + ']';
        case 'pair': return `(${show(x.first.get(), line)}, ${show(x.second.get(), line)})`;
        case 'enum': return 'code::fqcv_method::' + x.v;
        default: throw new CppError(`cannot print ${desc(x)}`, line);
      }
    }
    function streamOut(stream, x, line) {
      if (x.t === 'manip') {
        switch (x.name) {
          case 'endl': emit('\n', stream.err); break;
          case 'flush': break;
          case 'setw': out.width = x.n; break;
          case 'setprecision': out.prec = x.n; break;
          case 'boolalpha': out.boolalpha = true; break;
          case 'noboolalpha': out.boolalpha = false; break;
          case 'fixed': out.fixed = true; break;
          case 'defaultfloat': out.fixed = false; break;
          default: break;
        }
        return;
      }
      let s = show(x, line);
      if (out.width > s.length) s = ' '.repeat(out.width - s.length) + s;
      out.width = 0;
      emit(s, stream.err);
    }

    // ---- evaluation ----
    // ev(): the Cell of an expression (a fresh one for temporaries).
    const val = (node, env) => ev(node, env).get();

    function ev(node, env) {
      curLine = node.line || curLine;
      switch (node.k) {
        case 'lit': return new Cell(node.v);
        case 'var': {
          const c = env.lookup(node.name);
          if (c) return c;
          if (prog.funcs.has(node.name)) return new Cell({ t: 'func', decl: prog.funcs.get(node.name), env: top });
          if (/^(std::)?(map|set|unordered_map|array|cin)$/.test(node.name)) throw new CppError(`std::${node.name} is not supported in the playground`, node.line);
          throw new CppError(`'${node.name}' was not declared`, node.line);
        }
        case 'list': return new Cell({ t: 'list', items: node.items.map(i => new Cell(copy(val(i, env)))) });
        case 'lambda': return new Cell({ t: 'func', decl: { name: 'lambda', ret: T('auto'), params: node.params, body: node.body, line: node.line }, env });
        case 'cast': return new Cell(convert(val(node.e, env), node.type, node.line));
        case 'construct': return new Cell(construct(node.type, node.args.map(a => val(a, env)), node.line, node.args));
        case 'cond': return truthy(val(node.c, env), node.line) ? ev(node.a, env) : ev(node.b, env);
        case 'assign': return assign(node, env);
        case 'unary': return unary(node, env);
        case 'post': {
          const c = ev(node.e, env);
          const old = c.get();
          const n = num(old, node.line);
          c.set(old.t === 'double' ? dbl(n + (node.op === '++' ? 1 : -1)) : { t: old.t === 'char' ? 'char' : 'int', v: n + (node.op === '++' ? 1 : -1) });
          return new Cell(old);
        }
        case 'bin': return new Cell(binary(node, env));
        case 'index': {
          const base = ev(node.e, env);
          const b = base.get();
          const i = num(val(node.i, env), node.line, 'an index');
          if (b.t === 'string') return new CharCell(base, i, node.line);
          if (b.t === 'cstr') return new Cell({ t: 'char', v: b.v.charCodeAt(i) });
          if (b.t === 'vector' || b.t === 'list') {
            if (i < 0 || i >= b.items.length) throw new CppError(`index ${i} out of range (size ${b.items.length})`, node.line);
            return b.items[i];
          }
          throw new CppError(`${desc(b)} cannot be indexed`, node.line);
        }
        case 'member': return member(node, env);
        case 'call': return call(node, env);
        default: throw new CppError('unsupported expression', node.line);
      }
    }

    function member(node, env) {
      let base = ev(node.e, env);
      let b = base.get();
      if (node.arrow) {
        if (b.t !== 'optional') throw new CppError(`'->' needs a std::optional here, got ${desc(b)}`, node.line);
        if (b.v === null) throw new CppError('the optional is empty', node.line);
        b = b.v;
      }
      if (b.t === 'pair' && (node.name === 'first' || node.name === 'second')) return b[node.name];
      if (b.t === 'meas' && (node.name === 'meas_0' || node.name === 'meas_1')) return b[node.name];
      // a method used without a call
      return new Cell({ t: 'method', obj: node.arrow ? new Cell(b) : base, name: node.name });
    }

    function unary(node, env) {
      if (node.op === '++' || node.op === '--') {
        const c = ev(node.e, env);
        const x = c.get();
        const n = num(x, node.line) + (node.op === '++' ? 1 : -1);
        c.set(x.t === 'double' ? dbl(n) : { t: x.t === 'char' ? 'char' : 'int', v: n });
        return c;
      }
      const x = val(node.e, env);
      switch (node.op) {
        case '-': return new Cell(x.t === 'double' ? dbl(-x.v) : int(-num(x, node.line)));
        case '+': return new Cell(x.t === 'double' ? x : int(num(x, node.line)));
        case '!': return new Cell(bool(!truthy(x, node.line)));
        case '~':
          if (x.t === 'pset') return new Cell(pset(native(node.line, () => x.o.complement())));
          return new Cell(int(~num(x, node.line)));
        case '*':
          if (x.t !== 'optional') throw new CppError(`'*' needs a std::optional here, got ${desc(x)}`, node.line);
          if (x.v === null) throw new CppError('the optional is empty', node.line);
          return new Cell(x.v);
        default: throw new CppError('unsupported operator ' + node.op, node.line);
      }
    }

    function arith(op, a, b, line) {
      if (a.t === 'pset' || b.t === 'pset') {
        if (a.t !== 'pset' || b.t !== 'pset') throw new CppError(`'${op}' needs two psets, got ${desc(a)} and ${desc(b)}`, line);
        const f = { '|': 'union', '&': 'intersect', '-': 'minus', '^': 'xor', '*': 'mul' }[op];
        if (f) return pset(native(line, () => a.o[f](b.o)));
        if (op === '==') return bool(native(line, () => a.o.equals(b.o)));
        if (op === '!=') return bool(!native(line, () => a.o.equals(b.o)));
        throw new CppError(`'${op}' is not defined on psets`, line);
      }
      if ((isStr(a) || isStr(b)) && op === '+') {
        if (a.t === 'cstr' && b.t === 'cstr') throw new CppError('cannot add two string literals; make one a std::string', line);
        const s = x => (isStr(x) ? x.v : x.t === 'char' ? String.fromCharCode(x.v) : (() => { throw new CppError(`cannot add ${desc(x)} to a string`, line); })());
        return str(s(a) + s(b));
      }
      if (isStr(a) && isStr(b)) {
        switch (op) {
          case '==': return bool(a.v === b.v);
          case '!=': return bool(a.v !== b.v);
          case '<': return bool(a.v < b.v);
          case '>': return bool(a.v > b.v);
          case '<=': return bool(a.v <= b.v);
          case '>=': return bool(a.v >= b.v);
          default: break;
        }
      }
      if ((a.t === 'vector' && b.t === 'vector') && (op === '==' || op === '!=')) {
        const eq = a.items.length === b.items.length && a.items.every((c, i) => truthy(arith('==', c.get(), b.items[i].get(), line), line));
        return bool(op === '==' ? eq : !eq);
      }
      if (a.t === 'enum' && b.t === 'enum') {
        if (op === '==') return bool(a.v === b.v);
        if (op === '!=') return bool(a.v !== b.v);
      }
      const x = num(a, line), y = num(b, line);
      const isD = a.t === 'double' || b.t === 'double';
      const mk = v => (isD ? dbl(v) : int(v));
      switch (op) {
        case '+': return mk(x + y);
        case '-': return mk(x - y);
        case '*': return isD ? dbl(x * y) : int(Number(BigInt.asIntN(64, BigInt(Math.trunc(x)) * BigInt(Math.trunc(y)))));
        case '/':
          if (!isD && y === 0) throw new CppError('integer division by zero', line);
          return isD ? dbl(x / y) : int(x / y);
        case '%':
          if (isD) throw new CppError("'%' needs integers", line);
          if (y === 0) throw new CppError('integer division by zero', line);
          return int(x % y);
        case '<': return bool(x < y);
        case '>': return bool(x > y);
        case '<=': return bool(x <= y);
        case '>=': return bool(x >= y);
        case '==': return bool(x === y);
        case '!=': return bool(x !== y);
        case '&': return int(Number(BigInt.asIntN(64, BigInt(x) & BigInt(y))));
        case '|': return int(Number(BigInt.asIntN(64, BigInt(x) | BigInt(y))));
        case '^': return int(Number(BigInt.asIntN(64, BigInt(x) ^ BigInt(y))));
        case '<<': return int(Number(BigInt.asIntN(64, BigInt(x) << BigInt(y))));
        case '>>': return int(Number(BigInt.asIntN(64, BigInt(x) >> BigInt(y))));
        default: throw new CppError('unsupported operator ' + op, line);
      }
    }

    function binary(node, env) {
      if (node.op === '&&') return bool(truthy(val(node.a, env), node.line) && truthy(val(node.b, env), node.line));
      if (node.op === '||') return bool(truthy(val(node.a, env), node.line) || truthy(val(node.b, env), node.line));
      const a = val(node.a, env);
      if (node.op === '<<' && a.t === 'ostream') {
        streamOut(a, val(node.b, env), node.line);
        return a;
      }
      return arith(node.op, a, val(node.b, env), node.line);
    }

    function assign(node, env) {
      const c = ev(node.left, env);
      const old = c.get();
      let rhs = val(node.right, env);
      let v;
      if (node.op === '=') {
        const target = c instanceof CharCell ? T('char') : typeOfValue(old);
        v = convert(rhs, target, node.line);
      } else {
        const op = node.op.slice(0, -1);
        if (op === '+' && old.t === 'string') v = str(old.v + jsString(rhs, node.line));
        else if (old.t === 'pset') v = arith(op, old, rhs, node.line);
        else {
          v = arith(op, old, rhs, node.line);
          v = convert(v, typeOfValue(old), node.line);
        }
      }
      if (old.t === 'pset' || old.t === 'vector') release(old);
      c.set(v);
      return c;
    }

    // The declared type behind a stored value, for assignments.
    function typeOfValue(x) {
      switch (x.t) {
        case 'vector': return T('vector', [x.et || T('auto')]);
        case 'optional': return T('optional', [T('auto')]);
        case 'pair': return T('pair', [T('auto'), T('auto')]);
        case 'cstr': return T('string');
        case 'enum': return T('fqcv_method');
        case 'func': case 'builtin': return T('function');
        default: return T(x.t);
      }
    }

    function construct(type, args, line) {
      const n = type.name;
      if (n === 'string') {
        if (args.length === 0) return str('');
        if (args.length === 1) return str(jsString(args[0], line));
        if (args.length === 2) return str(String.fromCharCode(num(args[1], line)).repeat(Math.max(0, num(args[0], line))));
      }
      if (n === 'vector') {
        const et = type.args[0];
        if (args.length === 0) return { t: 'vector', et, items: [] };
        if (args.braced || args.isList) return { t: 'vector', et, items: args.map(a => new Cell(convert(a, et, line))) };
        if (args.length <= 2 && isNum(args[0]) && args[0].t !== 'char') {
          const count = num(args[0], line);
          const fill = args.length === 2 ? args[1] : defaultValue(et, line);
          return { t: 'vector', et, items: Array.from({ length: count }, () => new Cell(convert(fill, et, line))) };
        }
        if (args.length === 1 && (args[0].t === 'vector' || args[0].t === 'list')) return convert(args[0], type, line);
      }
      if (n === 'pset') {
        if (args.length === 0) return pset(native(line, () => new PSet()));
        if (args.length === 1 && args[0].t === 'pset') return copy(args[0]);
      }
      if (n === 'context') {
        if (Context.alive()) throw new CppError('another spbdd::context is still alive; only one can exist at a time', line);
        const ints = args.filter(a => isNum(a)).map(a => num(a, line));
        if (ints.length === 1) return { t: 'context', o: native(line, () => new Context(ints[0])) };
        if (ints.length === 2) return { t: 'context', o: native(line, () => new Context(ints[0], ints[1])) };
        throw new CppError('spbdd::context takes the number of qubits, or the numbers of data qubits and ancillas', line);
      }
      if (n === 'code') {
        if (args.length < 2 || args[0].t !== 'context') throw new CppError('spbdd::code takes (context, generators[, distance])', line);
        const gens = jsStrings(args[1], line, 'the generators');
        const o = args.length >= 3 ? native(line, () => new Code(args[0].o, gens, num(args[2], line)))
                                   : native(line, () => new Code(args[0].o, gens));
        return { t: 'code', o };
      }
      if (args.length === 0) return defaultValue(type, line);
      if (args.length === 1) return convert(args[0], type, line);
      if (n === 'pair' && args.length === 2) return { t: 'pair', first: new Cell(convert(args[0], type.args[0], line)), second: new Cell(convert(args[1], type.args[1], line)) };
      throw new CppError(`cannot construct ${typeName(type)} from these arguments`, line);
    }

    function defaultValue(type, line) {
      switch (type.name) {
        case 'int': return int(0);
        case 'double': return dbl(0);
        case 'bool': return bool(false);
        case 'char': return { t: 'char', v: 0 };
        case 'string': return str('');
        case 'vector': return { t: 'vector', et: type.args[0], items: [] };
        case 'pset': return pset(native(line, () => new PSet()));
        case 'optional': case 'violation': return { t: 'optional', v: null };
        case 'pair': return { t: 'pair', first: new Cell(defaultValue(type.args[0], line)), second: new Cell(defaultValue(type.args[1], line)) };
        case 'meas': return { t: 'meas', meas_0: new Cell(defaultValue(T('pset'), line)), meas_1: new Cell(defaultValue(T('pset'), line)) };
        case 'auto': throw new CppError("a variable declared 'auto' needs an initializer", line);
        default: throw new CppError(`${typeName(type)} has no default value here; initialize it`, line);
      }
    }

    // ---- calls ----
    function call(node, env) {
      // methods: obj.name(args)
      if (node.f.k === 'member') {
        const objCell = node.f.arrow ? (() => {
          const o = val(node.f.e, env);
          if (o.t !== 'optional' || o.v === null) throw new CppError("'->' needs a non-empty std::optional", node.line);
          return new Cell(o.v);
        })() : ev(node.f.e, env);
        const args = node.args.map(a => ev(a, env));
        return method(objCell, node.f.name, args, node.line, env);
      }
      const fc = ev(node.f, env);
      const f = fc.get();
      const args = node.args.map(a => ev(a, env));
      return invoke(f, args, node.line);
    }

    function invoke(f, argCells, line) {
      if (f.t === 'builtin') return new Cell(f.fn(argCells.map(c => c.get()), line));
      if (f.t === 'method') return method(f.obj, f.name, argCells, line);
      if (f.t !== 'func') throw new CppError(`${desc(f)} is not a function`, line);
      const d = f.decl;
      if (!d.body) throw new CppError(`'${d.name}' is declared but never defined`, line);
      if (argCells.length !== d.params.length) throw new CppError(`'${d.name}' takes ${d.params.length} argument(s), got ${argCells.length}`, line);
      if (++callDepth > 2000) throw new CppError('too much recursion', line);
      const fenv = new Env(f.env);
      d.params.forEach((p, i) => {
        if (p.type.ref && p.type.name !== 'auto' && !p.type.cnst) {
          fenv.define(p.name, argCells[i], false, line);
        } else if (p.type.ref && p.type.cnst && argCells[i].get().t === (p.type.name === 'string' ? 'string' : p.type.name)) {
          fenv.define(p.name, argCells[i], false, line);           // const T& binds without a copy
        } else if (p.type.ref && p.type.name === 'auto') {
          fenv.define(p.name, argCells[i], false, line);
        } else {
          fenv.define(p.name, new Cell(convert(argCells[i].get(), p.type, line)), true, line);
        }
      });
      let result = VOID;
      try {
        execBlock(d.body, fenv, true);
        if (d.ret && d.ret.name !== 'void' && d.ret.name !== 'auto' && d.name !== 'main') {
          throw new CppError(`'${d.name}' reached its end without returning a value`, d.line);
        }
      } catch (s) {
        if (!(s instanceof Signal) || s.kind !== 'return') throw s;
        result = s.value;
      } finally {
        leave(fenv);
        callDepth--;
      }
      if (result === VOID && d.name === 'main') return new Cell(int(0));   // main may omit its return
      if (d.ret && d.ret.ref && result instanceof Cell) return result;
      const v = result instanceof Cell ? result.get() : result;
      return new Cell(d.ret && d.ret.name !== 'auto' && d.ret.name !== 'void' ? convert(v, d.ret, line) : v);
    }

    function method(objCell, name, argCells, line) {
      const o = objCell.get();
      const a = argCells.map(c => c.get());
      const need = (k, what) => { if (a.length !== k) throw new CppError(`${what || name}() takes ${k} argument(s), got ${a.length}`, line); };
      const R = v => new Cell(v);
      switch (o.t) {
        case 'pset': {
          const s = o.o;
          const gates1 = ['x', 'y', 'z', 'h', 's', 'sdg', 'sx', 'sxdg'];
          const gates2 = ['cx', 'cy', 'cz', 'swap'];
          if (gates1.includes(name)) { need(1); native(line, () => s[name](num(a[0], line))); return objCell; }
          if (gates2.includes(name)) { need(2); native(line, () => s[name](num(a[0], line), num(a[1], line))); return objCell; }
          switch (name) {
            case 'reset': need(1); native(line, () => s.reset(jsInts(a[0], line, 'reset'))); return objCell;
            case 'fault_inject': need(1); return R(pset(native(line, () => s.fault_inject(jsInts(a[0], line, 'fault_inject')))));
            case 'measure_z': case 'measure_x': case 'measure_y': {
              const q = num(a[0], line);
              if (a.length === 2) return R(pset(native(line, () => s[name](q, truthy(a[1], line) ? 1 : 0))));
              need(1);
              const m = native(line, () => s[name](q, undefined));
              return R({ t: 'meas', meas_0: new Cell(pset(m.meas_0)), meas_1: new Cell(pset(m.meas_1)) });
            }
            case 'is_empty': need(0); return R(bool(native(line, () => s.is_empty())));
            case 'contains': need(1); return R(bool(native(line, () => s.contains(jsString(a[0], line)))));
            case 'subset_of': need(1); if (a[0].t !== 'pset') throw new CppError('subset_of() takes a pset', line); return R(bool(native(line, () => s.subset_of(a[0].o))));
            case 'size': need(0); return R(dbl(native(line, () => s.size())));
            case 'node_count': need(0); return R(int(native(line, () => s.node_count())));
            case 'hash': need(0); return R(int(native(line, () => s.hash())));
            case 'any_element': {
              need(0);
              const e = native(line, () => s.any_element());
              return R({ t: 'optional', v: e === null ? null : str(e) });
            }
            case 'for_each_element': {
              need(1);
              const fn = a[0];
              native(line, () => s.for_each_element(e => {
                const r = invoke(fn, [new Cell(str(e))], line).get();
                return r.t === 'void' ? true : truthy(r, line);
              }));
              return R(VOID);
            }
            default: break;
          }
          break;
        }
        case 'context': {
          const c = o.o;
          const S = (k, f) => { need(k); return R(pset(native(line, f))); };
          switch (name) {
            case 'empty': case 'all': case 'identity': return S(0, () => c[name]());
            case 'from_str': return S(1, () => c.from_str(jsString(a[0], line)));
            case 'from_list': return S(1, () => c.from_list(jsStrings(a[0], line)));
            case 'generated_by': return S(1, () => c.generated_by(jsStrings(a[0], line)));
            case 'coset_of':
              need(2);
              if (a[1].t === 'pset') return R(pset(native(line, () => c.coset_of_set(jsString(a[0], line), a[1].o))));
              return R(pset(native(line, () => c.coset_of(jsString(a[0], line), jsStrings(a[1], line)))));
            case 'identity_on': return S(1, () => c.identity_on(jsInts(a[0], line)));
            case 'pauli_at': return S(2, () => c.pauli_at(num(a[0], line), jsString(a[1], line)));
            case 'matching': return S(1, () => c.matching(jsString(a[0], line)));
            case 'weight_exactly': case 'weight_at_most':
              if (a.length === 2) return R(pset(native(line, () => c[name + '_on'](num(a[0], line), jsInts(a[1], line)))));
              return S(1, () => c[name](num(a[0], line)));
            case 'weight_between':
              if (a.length === 3) return R(pset(native(line, () => c.weight_between_on(num(a[0], line), num(a[1], line), jsInts(a[2], line)))));
              return S(2, () => c.weight_between(num(a[0], line), num(a[1], line)));
            case 'commute_with':
              need(1);
              return R(pset(native(line, () => c.commute_with(isStr(a[0]) ? a[0].v : jsStrings(a[0], line)))));
            case 'num_qubits': case 'num_data': case 'num_ancillas': case 'num_vars':
              need(0); return R(int(native(line, () => c[name]())));
            case 'live_nodes': case 'peak_nodes': case 'table_size': need(0); return R(int(native(line, () => c[name]())));
            case 'grow_num_qubits_to': need(1); native(line, () => c.grow_num_qubits_to(num(a[0], line))); return R(VOID);
            case 'set_dynamic_reordering': need(1); native(line, () => c.set_dynamic_reordering(truthy(a[0], line))); return R(VOID);
            case 'dynamic_reordering': need(0); return R(bool(native(line, () => c.dynamic_reordering())));
            case 'reorder_now': need(0); native(line, () => c.reorder_now()); return R(VOID);
            default: break;
          }
          break;
        }
        case 'code': {
          const c = o.o;
          switch (name) {
            case 'n': case 'k': case 'r': case 'd': need(0); return R(int(native(line, () => c[name]())));
            case 'stabilizer_generators': case 'logical_X_operators': case 'logical_Z_operators':
              need(0); return R(strVector(native(line, () => c[name]())));
            case 'S': case 'N': case 'NmS': need(0); return R(pset(native(line, () => c[name]())));
            case 'syndrome_of': case 'logical_component_of':
              need(1);
              if (!isStr(a[0])) throw new CppError(`${name}() takes a Pauli string here`, line);
              return R(boolVector(native(line, () => c[name](a[0].v))));
            case 'by_syndrome': case 'by_logical_component':
              need(1); return R(pset(native(line, () => c[name](bits(a[0], line)))));
            case 'find_qec_cond_violation': {
              if (a.length < 1 || a.length > 2 || a[0].t !== 'pset') throw new CppError('find_qec_cond_violation() takes (pset[, code::fqcv_method])', line);
              if (a.length === 2 && a[1].t !== 'enum') throw new CppError('the method is code::fqcv_method::bruteforce, table, image or square', line);
              const v = native(line, () => c.find_qec_cond_violation(a[0].o, a.length === 2 ? a[1].v : undefined));
              return R({ t: 'optional', v: v === null ? null : { t: 'pair', first: new Cell(str(v[0])), second: new Cell(str(v[1])) } });
            }
            default: break;
          }
          break;
        }
        case 'string': case 'cstr': {
          const s = o.v;
          switch (name) {
            case 'size': case 'length': need(0); return R(int(s.length));
            case 'empty': need(0); return R(bool(s.length === 0));
            case 'substr': {
              const pos = num(a[0], line), len = a.length > 1 ? num(a[1], line) : s.length;
              if (pos > s.length) throw new CppError('substr: position out of range', line);
              return R(str(s.substr(pos, len < 0 ? s.length : len)));
            }
            case 'find': {
              need(1);
              const i = s.indexOf(jsString(a[0], line));
              return R(int(i));
            }
            case 'push_back': need(1); objCell.set(str(s + jsString(a[0], line))); return R(VOID);
            case 'pop_back': need(0); objCell.set(str(s.slice(0, -1))); return R(VOID);
            case 'back': need(0); return R({ t: 'char', v: s.charCodeAt(s.length - 1) });
            case 'front': need(0); return R({ t: 'char', v: s.charCodeAt(0) });
            case 'at': need(1); return new CharCell(objCell, num(a[0], line), line);
            case 'clear': need(0); objCell.set(str('')); return R(VOID);
            case 'c_str': need(0); return R(str(s));
            default: break;
          }
          break;
        }
        case 'vector': case 'list': {
          const items = o.items;
          switch (name) {
            case 'size': need(0); return R(int(items.length));
            case 'empty': need(0); return R(bool(items.length === 0));
            case 'push_back': need(1); items.push(new Cell(convert(a[0], o.et, line))); return R(VOID);
            case 'emplace_back':
              items.push(new Cell(a.length === 1 ? convert(a[0], o.et, line) : construct(o.et || T('auto'), a, line)));
              return items[items.length - 1];
            case 'pop_back': need(0); if (!items.length) throw new CppError('pop_back() on an empty vector', line); release(items.pop().get()); return R(VOID);
            case 'back': case 'front':
              need(0);
              if (!items.length) throw new CppError(`${name}() on an empty vector`, line);
              return name === 'back' ? items[items.length - 1] : items[0];
            case 'at': {
              need(1);
              const i = num(a[0], line);
              if (i < 0 || i >= items.length) throw new CppError(`at(${i}): out of range (size ${items.length})`, line);
              return items[i];
            }
            case 'clear': need(0); items.forEach(c => release(c.get())); items.length = 0; return R(VOID);
            case 'resize': {
              const k = num(a[0], line);
              while (items.length > k) release(items.pop().get());
              while (items.length < k) items.push(new Cell(a.length > 1 ? convert(a[1], o.et, line) : defaultValue(o.et, line)));
              return R(VOID);
            }
            case 'reserve': return R(VOID);
            default: break;
          }
          break;
        }
        case 'optional':
          switch (name) {
            case 'has_value': need(0); return R(bool(o.v !== null));
            case 'value':
              need(0);
              if (o.v === null) throw new CppError('bad_optional_access: the optional is empty', line);
              return R(o.v);
            case 'reset': need(0); objCell.set({ t: 'optional', v: null }); return R(VOID);
            default: break;
          }
          break;
        default: break;
      }
      throw new CppError(`${desc(o)} has no member function '${name}' in the playground`, line);
    }

    // ---- statements ----
    function execBlock(block, env, sameScope) {
      const scope = sameScope ? env : new Env(env);
      try {
        for (const s of block.body) exec(s, scope);
      } finally {
        if (!sameScope) leave(scope);
      }
    }

    function declare(s, env) {
      for (const d of s.decls) {
        curLine = s.line;
        if (s.type.ref) {
          if (!d.init) throw new CppError(`the reference '${d.name}' must be initialized`, s.line);
          const c = ev(d.init, env);
          env.define(d.name, s.type.cnst ? c : c, false, s.line);
          continue;
        }
        let v;
        if (d.args) {
          const args = d.args.map(a => val(a, env));
          args.braced = !!d.args.braced;
          if (s.type.name === 'auto') {
            if (args.length !== 1) throw new CppError("'auto' needs exactly one initializer", s.line);
            v = convert(args[0], s.type, s.line);
          } else v = construct(s.type, args, s.line);
        } else if (d.init) {
          const init = val(d.init, env);
          if (init.t === 'list' && s.type.name !== 'auto' && s.type.name !== 'vector') {
            const args = init.items.map(c => c.get());
            args.braced = true;
            v = construct(s.type, args, s.line);
          } else if (init.t === 'context' && s.type.name === 'auto') {
            throw new CppError('spbdd::context cannot be copied', s.line);
          } else v = convert(init, s.type, s.line);
        } else v = defaultValue(s.type, s.line);
        const cell = new Cell(v);
        if (v.t === 'context' && !v.owner) v.owner = cell;
        env.define(d.name, cell, true, s.line);
      }
    }

    function bindNames(names, value, env, line, byRef) {
      let parts;
      if (value.t === 'pair') parts = [value.first, value.second];
      else if (value.t === 'meas') parts = [value.meas_0, value.meas_1];
      else throw new CppError(`structured bindings work on std::pair and meas_result here, not ${desc(value)}`, line);
      if (names.length !== parts.length) throw new CppError(`expected ${parts.length} names in the binding`, line);
      names.forEach((n, i) => env.define(n, byRef ? parts[i] : new Cell(copy(parts[i].get())), !byRef, line));
    }

    function loopBody(body, env) {
      try {
        if (body) exec(body, env);
      } catch (s) {
        if (s instanceof Signal && s.kind === 'break') return 'break';
        if (s instanceof Signal && s.kind === 'continue') return 'continue';
        throw s;
      }
      return null;
    }

    function exec(s, env) {
      curLine = s.line;
      switch (s.k) {
        case 'block': return execBlock(s, env, false);
        case 'expr': ev(s.e, env); return;
        case 'decl': return declare(s, env);
        case 'bind': {
          let c = ev(s.init, env);
          let v = c.get();
          if (v.t === 'optional') {
            if (v.v === null) throw new CppError('the optional is empty', s.line);
            v = v.v;
          }
          return bindNames(s.names, v, env, s.line, s.type.ref);
        }
        case 'if': {
          const scope = new Env(env);
          try {
            if (s.init) exec(s.init, scope);
            if (truthy(val(s.cond, scope), s.line)) exec(s.then, scope);
            else if (s.els) exec(s.els, scope);
          } finally { leave(scope); }
          return;
        }
        case 'while':
          while (truthy(val(s.cond, env), s.line)) {
            const scope = new Env(env);
            let r;
            try { r = loopBody(s.body, scope); } finally { leave(scope); }
            if (r === 'break') break;
          }
          return;
        case 'do':
          do {
            const scope = new Env(env);
            let r;
            try { r = loopBody(s.body, scope); } finally { leave(scope); }
            if (r === 'break') break;
          } while (truthy(val(s.cond, env), s.line));
          return;
        case 'for': {
          const scope = new Env(env);
          try {
            if (s.init) exec(s.init, scope);
            while (!s.cond || truthy(val(s.cond, scope), s.line)) {
              const inner = new Env(scope);
              let r;
              try { r = loopBody(s.body, inner); } finally { leave(inner); }
              if (r === 'break') break;
              if (s.step) ev(s.step, scope);
            }
          } finally { leave(scope); }
          return;
        }
        case 'rangefor': {
          const range = val(s.range, env);
          let cells;
          if (range.t === 'vector' || range.t === 'list') cells = range.items.slice();
          else if (isStr(range)) {
            const owner = new Cell(range);
            cells = [...range.v].map((_, i) => new CharCell(owner, i, s.line));
          } else throw new CppError(`cannot loop over ${desc(range)}`, s.line);
          for (const cell of cells) {
            const scope = new Env(env);
            let r;
            try {
              if (s.names) bindNames(s.names, cell.get(), scope, s.line, s.type.ref);
              else if (s.type.ref) scope.define(s.name, cell, false, s.line);
              else scope.define(s.name, new Cell(convert(cell.get(), s.type, s.line)), true, s.line);
              r = loopBody(s.body, scope);
            } finally { leave(scope); }
            if (r === 'break') break;
          }
          return;
        }
        // The value is copied before the function's locals are destroyed.
        case 'return': throw new Signal('return', s.e ? new Cell(copy(val(s.e, env))) : VOID);
        case 'break': throw new Signal('break');
        case 'continue': throw new Signal('continue');
        default: throw new CppError('unsupported statement', s.line);
      }
    }

    // ---- run ----
    let exitCode = 0;
    try {
      for (const g of prog.globals) exec(g, top);
      const main = prog.funcs.get('main');
      if (!main) throw new CppError('there is no int main()', toks[toks.length - 1].line);
      const r = invoke({ t: 'func', decl: main, env: top }, [], main.line).get();
      if (r.t === 'int') exitCode = r.v;
    } finally {
      if (out.buf) { write(out.buf, false); out.buf = ''; }
      leave(top);
    }
    return exitCode;
  }

  function errorMessage(M, e) {
    if (typeof e === 'number') {
      try { return M.exception_message(e); } catch (_) { return 'C++ exception'; }
    }
    return e && e.message ? e.message : String(e);
  }

  const api = { run, tokenize, Parser, CppError, formatDouble, errorMessage };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else global.SpbddCpp = api;
})(typeof self !== 'undefined' ? self : this);

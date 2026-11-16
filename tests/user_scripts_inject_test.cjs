// Runs the injection scripts written by user_scripts_test (argv[2] = their
// directory) in a minimal fake page, as WebView2 would at document creation.
"use strict";
const fs = require("fs");
const path = require("path");
const vm = require("vm");
const assert = require("assert");

const dir = process.argv[2];
const files = fs.readdirSync(dir).filter((f) => /^inject\d+\.js$/.test(f)).sort();
assert.ok(files.length >= 6, "injection scripts written");

function page(url, { framed = false, internal = false } = {}) {
  const listeners = {};
  const styles = [];
  const storage = new Map();
  const errors = [];
  const doc = {
    readyState: "loading",
    head: { appendChild: (n) => styles.push(n.textContent) },
    documentElement: { appendChild: (n) => styles.push(n.textContent) },
    body: null,
    createElement: () => ({ setAttribute() {} }),
    addEventListener: (t, f) => ((listeners[t] = listeners[t] || []).push(f)),
  };
  const u = new URL(url);
  const win = {
    __log: [],
    location: { href: url, protocol: u.protocol, hostname: internal ? "ulb.internal" : u.hostname },
    document: doc,
    localStorage: {
      getItem: (k) => (storage.has(k) ? storage.get(k) : null),
      setItem: (k, v) => storage.set(k, String(v)),
      removeItem: (k) => storage.delete(k),
      key: (i) => [...storage.keys()][i],
      get length() { return storage.size; },
    },
    console: { log() {}, info() {}, error: (...a) => errors.push(a.join(" ")) },
    setTimeout: (f) => { pending.push(f); return 1; },
    clearTimeout() {},
    MutationObserver: class { observe() {} },
    AbortController: class { abort() {} },
    fetch: () => Promise.reject(new Error("no network")),
    navigator: {},
  };
  const pending = [];
  win.window = win;
  win.top = framed ? {} : win;
  const ctx = vm.createContext(win);
  for (const f of files) {
    try { vm.runInContext(fs.readFileSync(path.join(dir, f), "utf8"), ctx, { filename: f }); }
    catch (e) { win.__log.push("syntax:" + f); }
  }
  // DOM ready, then idle work.
  doc.readyState = "interactive";
  (listeners.DOMContentLoaded || []).forEach((f) => f());
  while (pending.length) pending.shift()();
  return { log: win.__log, styles, errors };
}

const main = page("https://site.test/index.html");
assert.deepStrictEqual(main.log.filter((x) => !x.startsWith("syntax:")),
  ["start:loading", "get:1", "noframes", "idle:interactive", "info:idle", "excluded-ran"]);
assert.strictEqual(main.log.filter((x) => x.startsWith("syntax:")).length, 1, "only the broken script fails to parse");
assert.deepStrictEqual(main.styles, ["body{color:red}"]);
assert.ok(main.errors.some((e) => e.includes("thrower")), "a throwing script is reported, others still run");

const excluded = page("https://site.test/page2");
assert.ok(!excluded.log.includes("excluded-ran"), "@exclude");

const framed = page("https://site.test/", { framed: true });
assert.ok(!framed.log.includes("noframes") && framed.log.includes("start:loading"), "@noframes");

const other = page("https://other.test/");
assert.deepStrictEqual(other.log.filter((x) => !x.startsWith("syntax:")), ["other"]);

const internal = page("https://site.test/", { internal: true });
assert.deepStrictEqual(internal.log.filter((x) => !x.startsWith("syntax:")), [], "never on built-in pages");

console.log("user script injection tests passed");

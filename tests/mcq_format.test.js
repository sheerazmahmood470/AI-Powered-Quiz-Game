const assert = require("node:assert/strict");
const fs = require("node:fs");
const vm = require("node:vm");

const source = fs.readFileSync("frontend/script.js", "utf8");
const start = source.indexOf("function normalizeDisplayText(value) {");
const end = source.indexOf("// -------------------- QUESTION RENDERING", start);
assert(start >= 0 && end > start, "MCQ formatter functions must exist");

const context = { module: { exports: {} }, String, RegExp };
vm.runInNewContext(
  source.slice(start, end) + "\\nmodule.exports = { normalizeDisplayText, normalizeOptions };",
  context
);
const { normalizeDisplayText, normalizeOptions } = context.module.exports;

assert.equal(normalizeDisplayText("\\text{n} \\times \\text{sizeof}"), "n × sizeof");
assert.equal(normalizeDisplayText("n \\n bytes"), "n bytes");
assert.equal(normalizeDisplayText('printf("\\\\n")'), 'printf("\\\\n")');

const options = normalizeOptions(["A. first", "B) second", "C: third", "D- fourth"]);
assert.deepEqual(Array.from(options, item => item.letter), ["A", "B", "C", "D"]);
assert.deepEqual(Array.from(options, item => item.text), ["first", "second", "third", "fourth"]);

console.log("Frontend MCQ formatting tests: PASS");

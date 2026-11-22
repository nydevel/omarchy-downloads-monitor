// Tests for Model.js: node tests/test_model.mjs
import { readFileSync } from "node:fs"
import assert from "node:assert/strict"
import vm from "node:vm"

const source = readFileSync(new URL("../Model.js", import.meta.url), "utf8").replace(/^\.pragma library\s*/, "")
// QML strings have .arg(); emulate it inside the sandbox realm, where Model.js runs.
const ARG = `String.prototype.arg = function (v) {
  const m = this.match(/%(\\d)/g)
  if (!m) return String(this)
  const lowest = m.map(x => +x[1]).sort()[0]
  return this.replace(new RegExp("%" + lowest, "g"), String(v))
}
`
const M = {}
vm.runInNewContext(ARG + source + "\nObject.assign(out, {parseSnapshot, totalRate, fraction, formatBytes, formatRate, statusLine, notificationSafe, cleanProcessList})", { out: M })

const fmt = (n, d) => Number(n).toFixed(d || 0)
const tr = {
  bytes: n => M.formatBytes(n, fmt),
  number: fmt,
  t: k => ({ "status.progress": "%1 of %2", "status.eta": "about %1 left", "status.downloaded": "%1 downloaded",
    "status.writing": "writing", "status.estimated": "estimated", "status.active": "active" })[k] || k,
  tn: (k, n) => ({ "status.files": "%1 files", "time.seconds": "%1 sec", "time.minutes": "%1 min", "time.hours": "%1 h" })[k] || k
}

let tests = 0
function test(name, fn) { fn(); tests++ }

test("rejects junk", () => {
  assert.equal(M.parseSnapshot(""), null)
  assert.equal(M.parseSnapshot("{"), null)
  assert.equal(M.parseSnapshot('{"v":2,"jobs":[]}'), null)
  assert.equal(M.parseSnapshot("x".repeat(300000)), null)
})

test("sanitises jobs", () => {
  const snap = M.parseSnapshot(JSON.stringify({
    v: 1, rx: 10, finished: ["pacman", 7],
    jobs: [{ id: "a", kind: "evil", title: "t\u0007" + "y".repeat(400), detail: '<img src="https://example.invalid/x">', state: "weird",
      done: -5, total: 0, rate: "NaN", approx: "yes", files: 2.7 }]
  }))
  const j = snap.jobs[0]
  assert.equal(j.kind, "other")
  assert.equal(j.title.length, 160)
  assert.ok(!j.title.includes("\u0007"))
  assert.equal(j.detail, '<img src="https://example.invalid/x">') // shown via PlainText only
  assert.equal(j.state, "downloading")
  assert.equal(j.done, null)
  assert.equal(j.total, null)
  assert.equal(j.rate, 0)
  assert.equal(j.approx, false)
  assert.equal(j.files, 2)
  assert.deepEqual([...snap.finished], ["pacman", "7"])
})

test("caps job count", () => {
  const jobs = Array.from({ length: 50 }, (_, i) => ({ id: String(i), title: "x" }))
  assert.equal(M.parseSnapshot(JSON.stringify({ v: 1, jobs })).jobs.length, 20)
})

test("total rate counts estimated jobs once", () => {
  assert.equal(M.totalRate([{ rate: 100, approx: true }, { rate: 100, approx: true }, { rate: 5, approx: false }]), 100)
  assert.equal(M.totalRate([{ rate: 300, approx: false }, { rate: 100, approx: true }]), 300)
  assert.equal(M.totalRate([{ rate: 300, state: "writing" }]), 0)
})

test("formats", () => {
  assert.equal(M.formatBytes(0, fmt), "0 B")
  assert.equal(M.formatBytes(2100000, fmt), "2.1 MB")
  assert.equal(M.formatBytes(41e9, fmt), "41.0 GB")
  assert.equal(M.formatRate(2100000, fmt, true), "≈ 2.1 MB/s")
})

test("status lines", () => {
  assert.equal(M.statusLine({ state: "downloading", done: 500e6, total: 1e9, rate: 5e6, files: 1 }, tr), "500 MB of 1.0 GB · 50% · about 2 min left")
  assert.equal(M.statusLine({ state: "downloading", done: 12e6, total: null, rate: 1, files: 3 }, tr), "12.0 MB downloaded · 3 files")
  assert.equal(M.statusLine({ state: "downloading", done: null, total: null, approx: true }, tr), "estimated")
  assert.equal(M.statusLine({ state: "writing" }, tr), "writing")
})

test("notification text has no markup", () => {
  assert.equal(M.notificationSafe('<b>x</b> & "y"'), "bx/b  y")
})

test("process list setting", () => {
  assert.equal(M.cleanProcessList("transmission-gtk, qbittorrent  rm;rf $(x)"), "transmission-gt,qbittorrent")
  assert.equal(M.cleanProcessList(""), "")
})

console.log(`ok ${tests} tests`)

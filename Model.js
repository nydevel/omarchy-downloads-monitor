.pragma library

// Pure helpers for Panel.qml. Kept free of QML types so tests can load them
// in a plain JavaScript engine.

var MAX_LINE = 200000
var MAX_JOBS = 20
var MAX_TEXT = 160
var KINDS = ["pacman", "aur", "flatpak", "umu", "lutris", "heroic", "steam", "curl", "wget",
  "aria2", "git", "ytdlp", "pip", "npm", "cargo", "mise", "other"]

function text(value) {
  var s = String(value === undefined || value === null ? "" : value)
  // Drop control characters; display sinks are PlainText anyway.
  s = s.replace(/[\u0000-\u001f\u007f]/g, "")
  return s.length > MAX_TEXT ? s.slice(0, MAX_TEXT - 1) + "…" : s
}

function count(value) {
  var n = Number(value)
  return isFinite(n) && n >= 0 && n < 1e18 ? n : null
}

function parseSnapshot(line) {
  if (typeof line !== "string" || line.length === 0 || line.length > MAX_LINE) return null
  var raw
  try { raw = JSON.parse(line) } catch (e) { return null }
  if (!raw || typeof raw !== "object" || raw.v !== 1 || !Array.isArray(raw.jobs)) return null

  var jobs = []
  for (var i = 0; i < raw.jobs.length && jobs.length < MAX_JOBS; i++) {
    var j = raw.jobs[i]
    if (!j || typeof j !== "object") continue
    var total = count(j.total)
    jobs.push({
      id: text(j.id),
      kind: KINDS.indexOf(j.kind) >= 0 ? j.kind : "other",
      title: text(j.title) || "?",
      detail: text(j.detail),
      state: j.state === "writing" ? "writing" : "downloading",
      done: count(j.done),
      total: total && total > 0 ? total : null,
      rate: count(j.rate) || 0,
      approx: j.approx === true,
      files: Math.floor(count(j.files) || 0)
    })
  }

  var finished = []
  if (Array.isArray(raw.finished)) {
    for (var k = 0; k < raw.finished.length && finished.length < MAX_JOBS; k++) {
      var name = text(raw.finished[k])
      if (name) finished.push(name)
    }
  }
  return { rx: count(raw.rx) || 0, jobs: jobs, finished: finished }
}

function totalRate(jobs) {
  var sum = 0
  var approx = 0
  for (var i = 0; i < jobs.length; i++) {
    if (jobs[i].state === "writing") continue
    // Estimated jobs all report the same machine-wide rate; count it once.
    if (jobs[i].approx) approx = Math.max(approx, jobs[i].rate)
    else sum += jobs[i].rate
  }
  return Math.max(sum, approx)
}

function fraction(job) {
  if (!job || job.total === null || job.done === null) return -1
  return Math.max(0, Math.min(1, job.done / job.total))
}

function scaled(bytes) {
  var units = ["B", "KB", "MB", "GB", "TB"]
  var value = Math.max(0, Number(bytes) || 0)
  var unit = 0
  while (value >= 1000 && unit < units.length - 1) {
    value /= 1000
    unit++
  }
  var decimals = unit === 0 || value >= 100 ? 0 : 1
  return { value: value, unit: units[unit], decimals: decimals }
}

function formatBytes(bytes, formatNumber) {
  var s = scaled(bytes)
  return formatNumber(s.value, s.decimals) + " " + s.unit
}

function formatRate(rate, formatNumber, approx) {
  return (approx ? "≈ " : "") + formatBytes(rate, formatNumber) + "/s"
}

function formatDuration(seconds, tr) {
  seconds = Math.max(1, Math.round(seconds))
  if (seconds < 60) return tr.tn("time.seconds", seconds).arg(tr.number(seconds))
  var minutes = Math.round(seconds / 60)
  if (minutes < 60) return tr.tn("time.minutes", minutes).arg(tr.number(minutes))
  var hours = Math.floor(minutes / 60)
  var rest = minutes % 60
  var h = tr.tn("time.hours", hours).arg(tr.number(hours))
  return rest === 0 ? h : h + " " + tr.tn("time.minutes", rest).arg(tr.number(rest))
}

// tr: { bytes(n), number(n, decimals), t(key), tn(key, n) }
function statusLine(job, tr) {
  if (job.state === "writing") return tr.t("status.writing")
  var parts = []
  var f = fraction(job)
  if (f >= 0) {
    parts.push(tr.t("status.progress").arg(tr.bytes(job.done)).arg(tr.bytes(job.total)))
    parts.push(tr.number(Math.floor(f * 100)) + "%")
    if (job.rate > 0) parts.push(tr.t("status.eta").arg(formatDuration((job.total - job.done) / job.rate, tr)))
  } else if (job.done !== null) {
    parts.push(tr.t("status.downloaded").arg(tr.bytes(job.done)))
    if (job.files > 1) parts.push(tr.tn("status.files", job.files).arg(tr.number(job.files)))
  } else if (job.approx) {
    parts.push(tr.t("status.estimated"))
  } else {
    parts.push(tr.t("status.active"))
  }
  return parts.join(" · ")
}

// Notification bodies may be read as markup by the daemon; keep only text.
function notificationSafe(value) {
  return text(value).replace(/[<>&"']/g, "")
}

// Settings field: comma/space separated process names, each a plausible comm.
function cleanProcessList(value) {
  var out = []
  var items = String(value || "").split(/[\s,]+/)
  for (var i = 0; i < items.length && out.length < 32; i++) {
    var name = items[i].slice(0, 15)
    if (/^[A-Za-z0-9._+-]+$/.test(name) && out.indexOf(name) < 0) out.push(name)
  }
  return out.join(",")
}

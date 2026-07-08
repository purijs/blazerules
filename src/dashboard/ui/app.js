const $ = id => document.getElementById(id);
const fmt = n => Number(n || 0).toLocaleString(undefined, {maximumFractionDigits: 0});
const f2 = n => Number(n || 0).toLocaleString(undefined, {maximumFractionDigits: 2});
const dt = ms => ms ? new Date(ms).toLocaleString() : "";
const tm = ms => ms ? new Date(ms).toLocaleTimeString() : "";
const esc = s => String(s ?? "").replace(/[&<>"']/g, c => ({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;","'":"&#39;"}[c]));
const app = {decisionOffset: 0, decisionLimit: 800, filtered: false, yamlRaw: "", yamlLines: [], folded: new Set()};

function fmtBytes(n) {
  n = Math.max(0, Number(n || 0));
  const units = ["B", "KiB", "MiB", "GiB", "TiB"];
  let i = 0;
  while (n >= 1024 && i < units.length - 1) { n /= 1024; i++; }
  return `${n.toLocaleString(undefined, {maximumFractionDigits: i ? 1 : 0})} ${units[i]}`;
}

function cell(v, cls = "") { return `<td class="${cls}">${esc(v)}</td>`; }
function labels(o) { return Object.entries(o || {}).map(([k, v]) => `${k}=${v}`).join(", "); }
function visibleText(row) { return row.textContent.toLowerCase(); }
function applyFilter() {
  const q = $("filter").value.trim().toLowerCase();
  document.querySelectorAll("tbody tr").forEach(r => { r.style.display = !q || visibleText(r).includes(q) ? "" : "none"; });
}

function rows(obj, total = 0) {
  const entries = Object.entries(obj || {}).sort((a, b) => b[1] - a[1]);
  return entries.map(([k, v]) => `<tr>${cell(k, "mono")}${cell(fmt(v))}${cell(total ? f2(v / total * 100) + "%" : "")}</tr>`).join("") ||
    `<tr><td colspan="4" class="empty">No data</td></tr>`;
}

function setTabs() {
  document.querySelectorAll(".navbtn").forEach(b => b.onclick = () => {
    document.querySelectorAll(".navbtn,.view").forEach(x => x.classList.remove("active"));
    b.classList.add("active");
    $(b.dataset.tab).classList.add("active");
    applyFilter();
  });
  $("filter").oninput = applyFilter;
}

async function j(path) {
  const res = await fetch(path, {cache: "no-store"});
  return res.json();
}

function spark(points) {
  if (!points || !points.length) return "";
  const vals = points.map(p => p.bytes_per_sec || 0);
  const max = Math.max(...vals, 1);
  return points.map((p, i) => `${i / (points.length - 1 || 1) * 100},${88 - (vals[i] / max) * 78 - 5}`).join(" ");
}

function timeline(points) {
  if (!points || !points.length) return "";
  const vals = points.map(p => p.rps || 0);
  const max = Math.max(...vals, 1);
  return vals.map((v, i) => {
    const p = points[i] || {};
    return `<div class="tick" style="height:${Math.max(2, v / max * 88)}px" title="${fmt(v)} records/sec, ${fmtBytes(p.bytes_per_sec || 0)}/sec"></div>`;
  }).join("");
}

function planBars(obj, limit = 24) {
  const entries = Object.entries(obj || {}).sort((a, b) => b[1] - a[1]).slice(0, limit);
  const max = Math.max(...entries.map(x => x[1]), 1);
  return entries.map(([k, v]) => `
    <div class="opcard" title="${esc(k)}">
      <div class="opmeta"><span class="mono">${esc(k)}</span><strong>${fmt(v)}</strong></div>
      <div class="bar"><span style="width:${Math.min(100, v / max * 100)}%"></span></div>
    </div>`).join("") || `<div class="empty">No operator summary</div>`;
}

function updateSelect(id, counts) {
  const el = $(id);
  const value = el.value;
  const options = [`<option value="">Any</option>`].concat(
    Object.keys(counts || {}).sort().map(v => `<option value="${esc(v)}">${esc(v)}</option>`)
  );
  el.innerHTML = options.join("");
  if ([...el.options].some(o => o.value === value)) el.value = value;
}

function localMs(id) {
  const v = $(id).value;
  return v ? new Date(v).getTime() : 0;
}

function decisionQuery(scan) {
  const p = new URLSearchParams();
  p.set("limit", String(app.decisionLimit));
  p.set("offset", String(app.decisionOffset));
  if (scan) p.set("scan", "1");
  const decision = $("decisionFilter").value;
  const risk = $("riskFilter").value;
  const rule = $("ruleFilter").value.trim();
  const from = localMs("fromFilter");
  const to = localMs("toFilter");
  if (decision) p.set("decision", decision);
  if (risk) p.set("risk_band", risk);
  if (rule) p.set("rule", rule);
  if (from) p.set("from_ms", String(from));
  if (to) p.set("to_ms", String(to));
  return `/api/decisions?${p.toString()}`;
}

function renderDecisions(d) {
  const rowsOut = (d.rows || []).slice().reverse().map(x =>
    `<tr>${cell(dt(x.ts_ms))}${cell(x.matched ? "yes" : "no")}${cell(x.decision, "mono")}${cell(f2(x.score))}${cell(x.risk_band, "mono")}${cell(x.winning_rule_id, "mono")}${cell(x.ruleset_version, "mono")}</tr>`
  ).join("") || `<tr><td colspan="7" class="empty">No decision log rows match the current filters</td></tr>`;
  $("decisionRows").innerHTML = rowsOut;
  const start = (d.total_recent || 0) ? (app.decisionOffset + 1) : 0;
  const end = Math.min(app.decisionOffset + (d.rows || []).length, d.total_recent || 0);
  $("decisionPageInfo").textContent = `${fmt(start)}-${fmt(end)} of ${fmt(d.total_recent || 0)}`;
  $("prevDecisionPage").disabled = app.decisionOffset <= 0;
  $("nextDecisionPage").disabled = !d.has_more;
}

async function loadDecisions(scan) {
  const d = await j(decisionQuery(scan));
  renderDecisions(d);
  return d;
}

function setDecisionControls() {
  $("applyDecisionFilters").onclick = async () => {
    app.filtered = true;
    app.decisionOffset = 0;
    await loadDecisions(true);
  };
  $("resetDecisionFilters").onclick = async () => {
    app.filtered = false;
    app.decisionOffset = 0;
    ["decisionFilter", "riskFilter", "ruleFilter", "fromFilter", "toFilter"].forEach(id => $(id).value = "");
    await loadDecisions(false);
  };
  $("prevDecisionPage").onclick = async () => {
    app.decisionOffset = Math.max(0, app.decisionOffset - app.decisionLimit);
    await loadDecisions(app.filtered);
  };
  $("nextDecisionPage").onclick = async () => {
    app.decisionOffset += app.decisionLimit;
    await loadDecisions(app.filtered);
  };
}

function yamlIndent(line) {
  const m = line.match(/^ */);
  return m ? m[0].length : 0;
}

function setYaml(text) {
  if (text === app.yamlRaw) return;
  app.yamlRaw = text || "No active rules YAML configured. Start with --rules PATH.";
  app.yamlLines = app.yamlRaw.split("\n").map((text, index, arr) => {
    const indent = yamlIndent(text);
    let foldable = false;
    for (let i = index + 1; i < arr.length; ++i) {
      if (!arr[i].trim()) continue;
      foldable = yamlIndent(arr[i]) > indent;
      break;
    }
    return {text, indent, foldable};
  });
  app.folded.clear();
  renderYaml();
}

function renderYaml() {
  const hidden = new Array(app.yamlLines.length).fill(false);
  for (let i = 0; i < app.yamlLines.length; ++i) {
    if (!app.folded.has(i)) continue;
    const base = app.yamlLines[i].indent;
    for (let j = i + 1; j < app.yamlLines.length; ++j) {
      if (app.yamlLines[j].text.trim() && app.yamlLines[j].indent <= base) break;
      hidden[j] = true;
    }
  }
  $("activeYaml").innerHTML = app.yamlLines.map((line, i) => `
    <div class="yamlline ${hidden[i] ? "hidden" : ""}" data-line="${i}" style="--indent:${line.indent}">
      <span class="lineno">${i + 1}</span>
      ${line.foldable ? `<button class="fold" data-fold="${i}">${app.folded.has(i) ? "+" : "-"}</button>` : `<span class="foldspacer"></span>`}
      <span class="yamltext">${esc(line.text)}</span>
    </div>`).join("");
}

function setYamlControls() {
  $("activeYaml").onclick = e => {
    const btn = e.target.closest("[data-fold]");
    if (!btn) return;
    const i = Number(btn.dataset.fold);
    if (app.folded.has(i)) app.folded.delete(i); else app.folded.add(i);
    renderYaml();
  };
  $("collapseYaml").onclick = () => {
    app.folded = new Set(app.yamlLines.map((x, i) => x.foldable ? i : -1).filter(i => i >= 0));
    renderYaml();
  };
  $("expandYaml").onclick = () => {
    app.folded.clear();
    renderYaml();
  };
  $("copyYaml").onclick = async () => {
    const btn = $("copyYaml");
    try {
      await navigator.clipboard.writeText(app.yamlRaw || "");
      btn.textContent = "Copied";
      setTimeout(() => btn.textContent = "Copy", 1200);
    } catch {
      btn.textContent = "Copy failed";
      setTimeout(() => btn.textContent = "Copy", 1200);
    }
  };
}

async function refresh() {
  try {
    const [s, h, recent, r, e, m, rs] = await Promise.all([
      j("/api/summary"),
      j("/api/health"),
      app.filtered ? Promise.resolve(null) : j(`/api/decisions?limit=${app.decisionLimit}`),
      j("/api/rules?limit=200"),
      j("/api/errors?limit=300"),
      j("/api/metrics"),
      j("/api/ruleset")
    ]);
    $("topline").textContent = `version ${s.version || ""} | ruleset ${s.active_ruleset_version || "unknown"} | updated ${tm(s.last_update_ms)}`;
    const active = (h.sources || []).some(x => x.active);
    $("healthdot").className = "dot " + (active ? "ok" : "bad");
    $("healthtext").textContent = active ? "observing" : "no active sources";

    const o = s.overview || {};
    const t = s.timing_ms || {};
    const cards = [
      ["Records", fmt(o.records_evaluated)],
      ["Actioned", fmt(o.records_matched), f2((o.match_rate || 0) * 100) + "%"],
      ["Skipped", fmt(o.records_skipped)],
      ["Recent r/s", fmt(o.recent_records_per_sec)],
      ["Log size", fmtBytes(o.decision_log_bytes)],
      ["Data/s", `${fmtBytes(o.recent_bytes_per_sec)}/s`]
    ];
    $("cards").innerHTML = cards.map(c => `<div class="panel metric span2"><div class="label">${c[0]}</div><div class="value">${c[1]}</div><div class="hint">${c[2] || ""}</div></div>`).join("");

    const lat = [["total", t.total], ["evaluation", t.evaluation], ["transpose", t.transpose]];
    const latMax = Math.max(...lat.map(x => x[1] || 0), 1);
    $("latencyBars").innerHTML = o.has_metrics ? lat.map(x => `<div class="barrow"><span>${x[0]}</span><div class="bar"><span style="width:${Math.min(100, (x[1] || 0) / latMax * 100)}%"></span></div><span class="mono">${f2(x[1])} ms</span></div>`).join("") : `<div class="empty">No metrics endpoint configured for engine latency.</div>`;
    $("spark").innerHTML = `<polyline fill="none" stroke="#2563eb" stroke-width="2" points="${spark(s.history)}"></polyline>`;
    $("timelineBars").innerHTML = timeline(s.history);
    $("timelineHint").textContent = `${fmt(o.recent_records_per_sec)} r/s, ${fmtBytes(o.recent_bytes_per_sec)}/s`;

    const decisionTotal = Object.values(s.decision_counts || {}).reduce((a, b) => a + b, 0);
    $("decisionDist").innerHTML = rows(s.decision_counts, decisionTotal);
    updateSelect("decisionFilter", s.decision_counts || {});
    updateSelect("riskFilter", s.risk_band_counts || {});
    if (recent) renderDecisions(recent);
    $("logFacets").innerHTML = `<div class="facet"><strong>Decision</strong><table><tbody>${rows(s.decision_counts)}</tbody></table></div><div class="facet"><strong>Risk Band</strong><table><tbody>${rows(s.risk_band_counts)}</tbody></table></div>`;

    $("winningRows").innerHTML = Object.entries((recent?.rows || []).reduce((a, x) => {
      if (x.winning_rule_id) a[x.winning_rule_id] = (a[x.winning_rule_id] || 0) + 1;
      return a;
    }, {})).sort((a, b) => b[1] - a[1]).slice(0, 20).map(([k, v]) => `<tr>${cell(k, "mono")}${cell(fmt(v))}</tr>`).join("") || `<tr><td colspan="2" class="empty">No winning rules observed</td></tr>`;

    $("ruleRows").innerHTML = (r.rows || []).map(x => `<tr>${cell(x.rule_id, "mono")}${cell(fmt(x.fired_total))}${cell(f2((x.fire_rate || 0) * 100) + "%")}${cell(fmt(x.winning_total ?? x.winning_recent))}</tr>`).join("") || `<tr><td colspan="4" class="empty">No rule metrics yet</td></tr>`;
    $("operatorBars").innerHTML = planBars(rs.operator_counts, 24);
    $("rulesetSummary").innerHTML = `<div class="barrow"><span>active</span><span class="pill ${rs.active_valid ? "ok" : "bad"}">${rs.active_valid ? "valid" : "invalid"}</span><span>${fmt(rs.active_rule_count)} rules</span></div><div class="barrow"><span>candidate</span><span class="pill ${rs.candidate_valid ? "ok" : (rs.candidate_configured ? "bad" : "")}">${rs.candidate_configured ? (rs.candidate_valid ? "valid" : "invalid") : "none"}</span><span>${fmt(rs.candidate_rule_count)} rules</span></div><div class="muted mono">${esc(rs.active_error || rs.candidate_error || rs.active_path || "No rules path configured")}</div>`;
    $("actionRows").innerHTML = rows(rs.action_counts);
    $("versionRows").innerHTML = (rs.versions || []).map(x => `<tr>${cell(dt(x.modified_ms))}${cell(fmt(x.rule_count))}${cell(x.valid ? "yes" : "no")}${cell(x.path, "mono")}</tr>`).join("") || `<tr><td colspan="4" class="empty">No version directory configured</td></tr>`;
    setYaml(rs.active_yaml);
    $("diffRows").innerHTML = (rs.diff_rows || []).map(x => `<tr class="${x.kind === "added" ? "diff-add" : (x.kind === "removed" ? "diff-del" : "diff-chg")}">${cell(x.line)}${cell(x.kind)}${cell(x.active, "mono")}${cell(x.candidate, "mono")}</tr>`).join("") || `<tr><td colspan="4" class="empty">No candidate diff. Start with --candidate-rules PATH.</td></tr>`;

    $("errorCounts").innerHTML = rows(e.code_counts);
    $("errorRows").innerHTML = (e.rows || []).reverse().map(x => `<tr>${cell(dt(x.ts_ms))}${cell(x.code, "mono")}${cell(x.column_name, "mono")}${cell(x.message)}</tr>`).join("") || `<tr><td colspan="4" class="empty">No dead-letter rows. Start the agent with --dead-letter-path and this dashboard with --dead-letter-log.</td></tr>`;
    $("sourceRows").innerHTML = (h.sources || []).map(x => `<tr>${cell(x.name, "mono")}${cell(x.configured ? "yes" : "no")}${cell(x.active ? "yes" : "no")}${cell(x.location, "mono")}${cell(fmtBytes(x.bytes))}${cell(x.last_error)}</tr>`).join("");
    $("metricRows").innerHTML = (m.metrics || []).slice(0, 1500).map(x => `<tr>${cell(x.name, "mono")}${cell(f2(x.value))}${cell(labels(x.labels), "mono")}</tr>`).join("") || `<tr><td colspan="3" class="empty">No metrics endpoint configured or reachable</td></tr>`;
    applyFilter();
  } catch (err) {
    $("healthdot").className = "dot bad";
    $("healthtext").textContent = "poll failed";
    console.error(err);
  }
}

setTabs();
setDecisionControls();
setYamlControls();
refresh();
setInterval(refresh, 1000);

const $=id=>document.getElementById(id);
const fmt=n=>Number(n||0).toLocaleString(undefined,{maximumFractionDigits:0});
const f2=n=>Number(n||0).toLocaleString(undefined,{maximumFractionDigits:2});
const dt=ms=>ms?new Date(ms).toLocaleString():"";
const tm=ms=>ms?new Date(ms).toLocaleTimeString():"";
const esc=s=>String(s??"").replace(/[&<>"']/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;","'":"&#39;"}[c]));
function cell(v,cls=""){return `<td class="${cls}">${esc(v)}</td>`}
function rows(obj,total=0){let entries=Object.entries(obj||{}).sort((a,b)=>b[1]-a[1]);return entries.map(([k,v])=>`<tr>${cell(k,"mono")}${cell(fmt(v))}${cell(total?f2(v/total*100)+"%":"")}</tr>`).join("")||`<tr><td colspan="4" class="empty">No data</td></tr>`}
function labels(o){return Object.entries(o||{}).map(([k,v])=>`${k}=${v}`).join(", ")}
function visibleText(row){return row.textContent.toLowerCase()}
function applyFilter(){let q=$("filter").value.trim().toLowerCase();document.querySelectorAll("tbody tr").forEach(r=>{r.style.display=!q||visibleText(r).includes(q)?"":"none"})}
function setTabs(){document.querySelectorAll(".navbtn").forEach(b=>b.onclick=()=>{document.querySelectorAll(".navbtn,.view").forEach(x=>x.classList.remove("active"));b.classList.add("active");$(b.dataset.tab).classList.add("active");applyFilter()});$("filter").oninput=applyFilter}
function spark(points){if(!points||!points.length)return "";let vals=points.map(p=>p.rps||p.total_ms||0),max=Math.max(...vals,1);return points.map((p,i)=>`${i/(points.length-1||1)*100},${88-(vals[i]/max)*78-5}`).join(" ")}
function tableBars(obj,limit=16){let entries=Object.entries(obj||{}).sort((a,b)=>b[1]-a[1]).slice(0,limit),mx=Math.max(...entries.map(x=>x[1]),1);return entries.map(([k,v])=>`<div class="barrow"><span class="mono" title="${esc(k)}">${esc(k).slice(0,26)}</span><div class="bar"><span style="width:${Math.min(100,v/mx*100)}%"></span></div><span class="mono">${fmt(v)}</span></div>`).join("")||`<div class="empty">No data</div>`}
function timeline(points){if(!points||!points.length)return "";let vals=points.map(p=>p.rps||0),mx=Math.max(...vals,1);return vals.map(v=>`<div class="tick" style="height:${Math.max(2,v/mx*88)}px" title="${fmt(v)} r/s"></div>`).join("")}
async function j(path){let r=await fetch(path,{cache:"no-store"});return r.json()}
async function refresh(){
 try{
  const [s,h,d,r,e,b,m,rs]=await Promise.all([j("/api/summary"),j("/api/health"),j("/api/decisions?limit=800"),j("/api/rules?limit=200"),j("/api/errors?limit=300"),j("/api/benchmarks"),j("/api/metrics"),j("/api/ruleset")]);
  $("topline").textContent=`version ${s.version||""} | ruleset ${s.active_ruleset_version||"unknown"} | updated ${tm(s.last_update_ms)}`;
  let active=(h.sources||[]).some(x=>x.active); $("healthdot").className="dot "+(active?"ok":"bad"); $("healthtext").textContent=active?"observing":"no active sources";
  let o=s.overview||{}, t=s.timing_ms||{};
  const cards=[["Records",fmt(o.records_evaluated)],["Matched",fmt(o.records_matched),f2((o.match_rate||0)*100)+"% match"],["Skipped",fmt(o.records_skipped)],["Batches",fmt(o.batches_evaluated)],["Recent r/s",fmt(o.recent_records_per_sec)],["Avg score",f2(o.avg_score_recent)]];
  $("cards").innerHTML=cards.map(c=>`<div class="panel metric span2"><div class="label">${c[0]}</div><div class="value">${c[1]}</div><div class="hint">${c[2]||""}</div></div>`).join("");
  const lat=[["total",t.total],["evaluation",t.evaluation],["transpose",t.transpose]], mx=Math.max(...lat.map(x=>x[1]||0),1);
  $("latencyBars").innerHTML=lat.map(x=>`<div class="barrow"><span>${x[0]}</span><div class="bar"><span style="width:${Math.min(100,(x[1]||0)/mx*100)}%"></span></div><span class="mono">${f2(x[1])} ms</span></div>`).join("");
  $("spark").innerHTML=`<polyline fill="none" stroke="#2563eb" stroke-width="2" points="${spark(s.history)}"></polyline>`;
  $("timelineBars").innerHTML=timeline(s.history);
  let decisionTotal=Object.values(s.decision_counts||{}).reduce((a,b)=>a+b,0);
  $("decisionDist").innerHTML=rows(s.decision_counts,decisionTotal);
  $("winningRows").innerHTML=Object.entries((d.rows||[]).reduce((a,x)=>{if(x.winning_rule_id)a[x.winning_rule_id]=(a[x.winning_rule_id]||0)+1;return a},{})).sort((a,b)=>b[1]-a[1]).slice(0,20).map(([k,v])=>`<tr>${cell(k,"mono")}${cell(fmt(v))}</tr>`).join("")||`<tr><td colspan="2" class="empty">No winning rules observed</td></tr>`;
  $("decisionRows").innerHTML=(d.rows||[]).reverse().map(x=>`<tr>${cell(dt(x.ts_ms))}${cell(x.matched?"yes":"no")}${cell(x.decision,"mono")}${cell(f2(x.score))}${cell(x.risk_band,"mono")}${cell(x.winning_rule_id,"mono")}${cell(x.ruleset_version,"mono")}</tr>`).join("")||`<tr><td colspan="7" class="empty">No decision log rows</td></tr>`;
  $("logFacets").innerHTML=`<div class="facet"><strong>Decision</strong><table><tbody>${rows(s.decision_counts)}</tbody></table></div><div class="facet"><strong>Risk Band</strong><table><tbody>${rows(s.risk_band_counts)}</tbody></table></div>`;
  $("ruleRows").innerHTML=(r.rows||[]).map(x=>`<tr>${cell(x.rule_id,"mono")}${cell(fmt(x.fired_total))}${cell(f2((x.fire_rate||0)*100)+"%")}${cell(fmt(x.winning_recent))}</tr>`).join("")||`<tr><td colspan="4" class="empty">No rule metrics yet</td></tr>`;
  $("operatorBars").innerHTML=tableBars(rs.operator_counts,22);
  $("rulesetSummary").innerHTML=`<div class="barrow"><span>active</span><span class="pill ${rs.active_valid?"ok":"bad"}">${rs.active_valid?"valid":"invalid"}</span><span>${fmt(rs.active_rule_count)} rules</span></div><div class="barrow"><span>candidate</span><span class="pill ${rs.candidate_valid?"ok":(rs.candidate_configured?"bad":"")}">${rs.candidate_configured?(rs.candidate_valid?"valid":"invalid"):"none"}</span><span>${fmt(rs.candidate_rule_count)} rules</span></div><div class="muted mono">${esc(rs.active_error||rs.candidate_error||rs.active_path||"No rules path configured")}</div>`;
  $("actionRows").innerHTML=rows(rs.action_counts);
  $("versionRows").innerHTML=(rs.versions||[]).map(x=>`<tr>${cell(dt(x.modified_ms))}${cell(fmt(x.rule_count))}${cell(x.valid?"yes":"no")}${cell(x.path,"mono")}</tr>`).join("")||`<tr><td colspan="4" class="empty">No version directory configured</td></tr>`;
  $("activeYaml").textContent=rs.active_yaml||"No active rules YAML configured. Start with --rules PATH.";
  $("diffRows").innerHTML=(rs.diff_rows||[]).map(x=>`<tr class="${x.kind==="added"?"diff-add":(x.kind==="removed"?"diff-del":"diff-chg")}">${cell(x.line)}${cell(x.kind)}${cell(x.active,"mono")}${cell(x.candidate,"mono")}</tr>`).join("")||`<tr><td colspan="4" class="empty">No candidate diff. Start with --candidate-rules PATH.</td></tr>`;
  $("errorCounts").innerHTML=rows(e.code_counts);
  $("errorRows").innerHTML=(e.rows||[]).reverse().map(x=>`<tr>${cell(dt(x.ts_ms))}${cell(x.code,"mono")}${cell(x.column_name,"mono")}${cell(x.message)}</tr>`).join("")||`<tr><td colspan="4" class="empty">No dead-letter rows</td></tr>`;
  $("benchFormats").innerHTML=(b.formats||[]).map(x=>`<tr>${cell(x.format,"mono")}${cell(fmt(x.ok))}${cell(fmt(x.median_engine_rps))}${cell(f2(x.median_engine_gib_sec))}</tr>`).join("")||`<tr><td colspan="4" class="empty">No benchmark data</td></tr>`;
  $("benchRows").innerHTML=(b.rows||[]).slice(0,900).map(x=>`<tr>${cell(x.format,"mono")}${cell(fmt(x.actual_avg_json_bytes)+" B")}${cell(fmt(x.records))}${cell(fmt(x.batch_size))}${cell(x.status,"mono")}${cell(fmt(x.engine_rps))}${cell(f2(x.input_gib_per_sec))}${cell(f2(x.p95_ms))}</tr>`).join("");
  $("sourceRows").innerHTML=(h.sources||[]).map(x=>`<tr>${cell(x.name,"mono")}${cell(x.configured?"yes":"no")}${cell(x.active?"yes":"no")}${cell(x.location,"mono")}${cell(fmt(x.bytes))}${cell(x.last_error)}</tr>`).join("");
  $("metricRows").innerHTML=(m.metrics||[]).slice(0,1500).map(x=>`<tr>${cell(x.name,"mono")}${cell(f2(x.value))}${cell(labels(x.labels),"mono")}</tr>`).join("")||`<tr><td colspan="3" class="empty">No metrics endpoint configured or reachable</td></tr>`;
  applyFilter();
 }catch(err){$("healthdot").className="dot bad";$("healthtext").textContent="poll failed";console.error(err)}
}
setTabs(); refresh(); setInterval(refresh,1000);

/* ─── ncaseonetwo.xyz — app logic ───────────────────────────── */

// Configurable backend (Cloudflare Worker / serverless proxy that holds the
// GitHub PAT and dispatches the compile workflow). When unset, the UI falls
// back to a "Copy YAML + Open GitHub Actions" flow — no token ever in the
// browser.
const CONFIG = {
  // Set this to your Worker / serverless URL once deployed, e.g.:
  //   "https://ncaseonetwo-api.<your-worker>.workers.dev"
  // The worker must expose:
  //   POST /compile  { yaml, board, branch }       -> { run_id, html_url }
  //   GET  /status?run_id=...                      -> { status, conclusion, artifacts:[{name,url}] }
  apiBase: "",
  // Repo that hosts the compile workflow.
  compileRepo: "youkorr/ncaseonetwo-builds",
};

// ──────────────────────────────────────────────────────────────
// State
const state = {
  board: null,
  branch: "stable",
};

// ──────────────────────────────────────────────────────────────
// DOM helpers
const $ = (s) => document.querySelector(s);
const $$ = (s) => document.querySelectorAll(s);

// ──────────────────────────────────────────────────────────────
// Lightweight YAML loader (js-yaml from CDN, loaded lazily)
let YAML = null;
async function ensureYAML() {
  if (YAML) return YAML;
  await new Promise((res, rej) => {
    const s = document.createElement("script");
    s.src = "https://cdn.jsdelivr.net/npm/js-yaml@4.1.0/dist/js-yaml.min.js";
    s.onload = res; s.onerror = rej; document.head.appendChild(s);
  });
  YAML = window.jsyaml;
  return YAML;
}

// ──────────────────────────────────────────────────────────────
// Drop-zone + editor
const dz = $("#dropzone");
const fileInput = $("#file-input");
const editor = $("#editor");
const dzMeta = $("#dz-meta");

dz.addEventListener("click", () => fileInput.click());
fileInput.addEventListener("change", (e) => {
  const f = e.target.files[0]; if (f) loadFile(f);
});
["dragenter","dragover"].forEach(ev => dz.addEventListener(ev, e => {
  e.preventDefault(); dz.classList.add("border-neon-cyan/60");
}));
["dragleave","drop"].forEach(ev => dz.addEventListener(ev, e => {
  e.preventDefault(); dz.classList.remove("border-neon-cyan/60");
}));
dz.addEventListener("drop", e => {
  const f = e.dataTransfer.files[0]; if (f) loadFile(f);
});

function loadFile(file) {
  const reader = new FileReader();
  reader.onload = () => {
    editor.value = reader.result;
    dzMeta.textContent = `Loaded: ${file.name} · ${(file.size/1024).toFixed(1)} KB`;
    validate();
  };
  reader.readAsText(file);
}

editor.addEventListener("input", debounce(validate, 350));

$("#btn-clear").onclick  = () => { editor.value=""; dzMeta.textContent=""; $("#result").classList.add("hidden"); };
$("#btn-copy").onclick   = () => navigator.clipboard.writeText(editor.value);
$("#btn-format").onclick = async () => {
  try {
    const Y = await ensureYAML();
    const doc = Y.load(editor.value || "");
    editor.value = Y.dump(doc, { lineWidth: 120, noRefs: true });
    validate();
  } catch (e) { /* validation will show error */ }
};

// ──────────────────────────────────────────────────────────────
// YAML validation
async function validate() {
  const text = editor.value;
  const box = $("#result");
  if (!text.trim()) { box.classList.add("hidden"); return; }

  const Y = await ensureYAML();
  let doc, err;
  try { doc = Y.load(text); } catch (e) { err = e; }

  box.classList.remove("hidden","result-ok","result-warn","result-error");

  if (err) {
    box.classList.add("result-error");
    box.innerHTML = renderResult("error", "YAML parse error", [
      err.message || String(err)
    ]);
    return;
  }
  if (typeof doc !== "object" || doc === null) {
    box.classList.add("result-error");
    box.innerHTML = renderResult("error", "Document is not a mapping", ["Top-level must be a YAML mapping."]);
    return;
  }

  const warnings = [];
  const errors = [];
  if (!doc.esphome) errors.push("Missing required section: `esphome:`");
  else if (!doc.esphome.name) errors.push("Missing `esphome.name`");

  const chipKeys = ["esp32","esp8266","rp2040","host"];
  const hasChip = chipKeys.find(k => k in doc);
  if (!hasChip) errors.push("Missing chip section (one of: esp32, esp8266, rp2040, host)");

  if (!doc.api && !doc.mqtt && !doc.web_server)
    warnings.push("No control interface (`api`, `mqtt` or `web_server`)");
  if (!doc.logger) warnings.push("No `logger:` section — you won't see device logs");
  if (!doc.ota)    warnings.push("No `ota:` section — over-the-air updates disabled");
  if (!doc.wifi && !doc.ethernet) warnings.push("No `wifi:` nor `ethernet:` — device will not have network connectivity");

  const sections = Object.keys(doc).length;
  const linesNum = text.split("\n").length;

  let level = "ok";
  if (errors.length) level = "error";
  else if (warnings.length) level = "warn";

  box.classList.add("result-" + level);
  box.innerHTML = renderResult(level, levelTitle(level), errors, warnings, { sections, lines: linesNum });
}

function levelTitle(l){
  return l==="ok"   ? "YAML looks good ✓"
       : l==="warn" ? "YAML is valid · with warnings"
                    : "YAML has errors";
}

function renderResult(level, title, errors=[], warnings=[], stats=null) {
  const color = level==="error" ? "text-rose-400"
              : level==="warn"  ? "text-amber-300"
              :                   "text-lime-300";
  let html = `<div class="flex items-center gap-3 mb-3">
    <div class="text-sm font-semibold ${color}">${title}</div>`;
  if (stats) html += `<div class="ml-auto text-xs text-zinc-500">${stats.sections} sections · ${stats.lines} lines</div>`;
  html += `</div>`;
  if (errors.length) {
    html += `<ul class="space-y-1 mb-2">`;
    errors.forEach(e => html += `<li class="text-sm text-rose-300/90 flex gap-2"><span>✕</span><span>${escapeHTML(e)}</span></li>`);
    html += `</ul>`;
  }
  if (warnings.length) {
    html += `<ul class="space-y-1">`;
    warnings.forEach(w => html += `<li class="text-sm text-amber-200/80 flex gap-2"><span>⚠</span><span>${escapeHTML(w)}</span></li>`);
    html += `</ul>`;
  }
  if (!errors.length && !warnings.length) {
    html += `<p class="text-sm text-lime-200/80">No issues detected. Ready to compile.</p>`;
  }
  return html;
}

function escapeHTML(s){ return String(s).replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c])); }

// ──────────────────────────────────────────────────────────────
// Boards
const grid = $("#board-grid");
const chipSel = $("#board-chip");
const searchInp = $("#board-search");

const chips = [...new Set(BOARDS.map(b => b.chip))].sort();
chips.forEach(c => {
  const o = document.createElement("option");
  o.value = c; o.textContent = CHIP_LABEL[c] || c;
  chipSel.appendChild(o);
});

function renderBoards() {
  const q = searchInp.value.toLowerCase().trim();
  const chip = chipSel.value;
  grid.innerHTML = "";
  BOARDS
    .filter(b => !chip || b.chip === chip)
    .filter(b => !q || (b.name+" "+b.brand+" "+b.id).toLowerCase().includes(q))
    .forEach(b => {
      const el = document.createElement("div");
      el.className = "board-card";
      if (state.board && state.board.id === b.id) el.classList.add("selected");
      el.innerHTML = `
        <div class="flex items-start justify-between gap-2">
          <div>
            <div class="text-[11px] uppercase tracking-wider text-zinc-500">${b.brand}</div>
            <div class="font-semibold mt-0.5 leading-tight">${b.name}</div>
          </div>
          ${b.tag ? `<span class="tag-pill">${b.tag}</span>` : ""}
        </div>
        <div class="mt-3 flex flex-wrap gap-1.5">
          <span class="chip-pill">${CHIP_LABEL[b.chip] || b.chip}</span>
          <span class="chip-pill">${b.flash}</span>
          ${b.psram && b.psram !== "—" ? `<span class="chip-pill">PSRAM ${b.psram}</span>` : ""}
        </div>`;
      el.onclick = () => selectBoard(b);
      grid.appendChild(el);
    });
}

function selectBoard(b) {
  state.board = b;
  $("#sel-board").innerHTML = `<span class="text-zinc-100">${b.brand} · ${b.name}</span>
    <span class="block text-xs text-zinc-500 mt-0.5">${CHIP_LABEL[b.chip] || b.chip} · ${b.flash} flash · PSRAM ${b.psram}</span>`;
  // If editor empty, fill with template
  if (!editor.value.trim()) {
    editor.value = BOARD_TEMPLATE(b, { esphomeBranch: state.branch });
    validate();
  }
  renderBoards();
}

chipSel.addEventListener("change", renderBoards);
searchInp.addEventListener("input", debounce(renderBoards, 120));
renderBoards();

// ──────────────────────────────────────────────────────────────
// Branch toggle
$$(".branch-btn").forEach(b => b.addEventListener("click", () => {
  state.branch = b.dataset.branch;
  $$(".branch-btn").forEach(x => { x.classList.remove("active","bg-white/10"); x.classList.add("text-zinc-400"); });
  b.classList.add("active");
  b.classList.remove("text-zinc-400");
}));
$('.branch-btn[data-branch="stable"]').classList.add("active");

// ──────────────────────────────────────────────────────────────
// Compile
const log = $("#log");
const statusDot = $("#status-dot");
const statusLabel = $("#status-label");
const runLink = $("#run-link");

function setStatus(s, label) {
  statusDot.className = "w-2 h-2 rounded-full bg-zinc-600";
  if (s === "running") statusDot.classList.add("status-running");
  if (s === "ok")      statusDot.classList.add("status-ok");
  if (s === "err")     statusDot.classList.add("status-err");
  statusLabel.textContent = label;
}
function appendLog(line) {
  if (log.textContent === "Waiting for a build…") log.textContent = "";
  log.textContent += line + "\n";
  log.scrollTop = log.scrollHeight;
}

$("#btn-compile").onclick = async () => {
  if (!state.board) { alert("Pick a board first."); document.getElementById("boards").scrollIntoView({behavior:"smooth"}); return; }
  if (!editor.value.trim()) { alert("Paste or load a YAML first."); return; }

  // Re-validate hard before dispatching
  await validate();
  if ($("#result").classList.contains("result-error")) {
    alert("Fix YAML errors before compiling."); return;
  }

  log.textContent = "";
  setStatus("running", "queued…");
  appendLog(`▶ board    : ${state.board.brand} ${state.board.name} (${state.board.id})`);
  appendLog(`▶ chip     : ${CHIP_LABEL[state.board.chip] || state.board.chip}`);
  appendLog(`▶ esphome  : ${state.branch}`);
  appendLog(`▶ repo     : ${CONFIG.compileRepo}`);

  if (!CONFIG.apiBase) {
    appendLog("");
    appendLog("✖ No compile backend configured.");
    appendLog("  Set CONFIG.apiBase in app.js to your Cloudflare Worker URL,");
    appendLog("  or click the link below to trigger the workflow manually:");
    appendLog("");
    const url = `https://github.com/${CONFIG.compileRepo}/actions/workflows/compile.yml`;
    appendLog(`  → ${url}`);
    runLink.href = url; runLink.classList.remove("hidden");
    setStatus("err", "no backend");
    return;
  }

  try {
    const r = await fetch(CONFIG.apiBase + "/compile", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({
        yaml: editor.value,
        board: state.board.id,
        branch: state.branch,
      }),
    });
    if (!r.ok) throw new Error(`HTTP ${r.status}: ${await r.text()}`);
    const { run_id, html_url } = await r.json();
    appendLog(`✓ dispatched · run_id=${run_id}`);
    runLink.href = html_url; runLink.classList.remove("hidden");
    pollStatus(run_id);
  } catch (e) {
    appendLog("✖ " + e.message);
    setStatus("err", "failed");
  }
};

async function pollStatus(runId) {
  let dots = 0;
  while (true) {
    await sleep(4000);
    let s;
    try {
      const r = await fetch(`${CONFIG.apiBase}/status?run_id=${runId}`);
      s = await r.json();
    } catch (e) { appendLog("… retry"); continue; }
    setStatus("running", `${s.status}${".".repeat(++dots % 4)}`);
    if (s.status === "completed") {
      if (s.conclusion === "success") {
        setStatus("ok", "success");
        appendLog("");
        appendLog("✓ build successful");
        (s.artifacts || []).forEach(a => appendLog(`  ↓ ${a.name}: ${a.url}`));
      } else {
        setStatus("err", s.conclusion || "failed");
        appendLog("✖ build " + s.conclusion);
      }
      return;
    }
  }
}

// ──────────────────────────────────────────────────────────────
// utils
function debounce(fn, ms){ let t; return (...a)=>{ clearTimeout(t); t=setTimeout(()=>fn(...a), ms); }; }
function sleep(ms){ return new Promise(r => setTimeout(r, ms)); }

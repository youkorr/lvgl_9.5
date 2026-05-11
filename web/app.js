/* ─── ncaseonetwo.xyz — app logic ───────────────────────────── */

// Configurable backend (Cloudflare Worker / serverless proxy that holds the
// GitHub PAT and dispatches the compile workflow). When unset, the UI falls
// back to a "Copy YAML + Open GitHub Actions" flow — no token ever in the
// browser.
const CONFIG = {
  // Cloudflare Worker that holds the GitHub PAT and dispatches the compile
  // workflow. Set up in worker/worker.js — never put a token in this file.
  apiBase:     "https://ncaseonetwo-api.sapphire-younes.workers.dev",
  // Repo that hosts the compile workflow.
  compileRepo: "youkorr/ncaseonetwo-builds",
};

// ──────────────────────────────────────────────────────────────
// State
const state = {
  board: null,
  branch: "stable",
  runner: "self",   // "self" = self-hosted Unraid · "github" = GitHub-hosted fallback
  repo:   null,     // { full_name, html_url, description, stargazers_count, updated_at, language }
  extras: [],       // [{ name, content_b64, size }]
};
const EXTRAS_MAX_BYTES = 60 * 1024;

// ──────────────────────────────────────────────────────────────
// DOM helpers
const $ = (s) => document.querySelector(s);
const $$ = (s) => document.querySelectorAll(s);

// ──────────────────────────────────────────────────────────────
// Lightweight YAML loader (js-yaml from CDN, loaded lazily)
// We extend the default schema with the custom tags ESPHome uses so the
// parser doesn't choke on `!secret`, `!include`, `!lambda`, `!extend`,
// `!remove`, `!force` and similar markers.
let YAML = null;
let ESPHOME_SCHEMA = null;
async function ensureYAML() {
  if (YAML) return YAML;
  await new Promise((res, rej) => {
    const s = document.createElement("script");
    s.src = "https://cdn.jsdelivr.net/npm/js-yaml@4.1.0/dist/js-yaml.min.js";
    s.onload = res; s.onerror = rej; document.head.appendChild(s);
  });
  YAML = window.jsyaml;

  const tagNames = ["!secret","!include","!include_dir_list","!include_dir_merge_list",
                    "!include_dir_named","!include_dir_merge_named","!lambda",
                    "!extend","!remove","!force"];
  const types = [];
  for (const name of tagNames) {
    for (const kind of ["scalar","sequence","mapping"]) {
      types.push(new YAML.Type(name, {
        kind,
        construct: (data) => ({ __tag: name, value: data }),
      }));
    }
  }
  ESPHOME_SCHEMA = YAML.DEFAULT_SCHEMA.extend(types);
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

// ──────────────────────────────────────────────────────────────
// Extra files (partition CSVs, fonts, images, includes…)
const extrasInput    = $("#extras-input");
const extrasAddBtn   = $("#extras-add");
const extrasDropzone = $("#extras-dropzone");
const extrasList     = $("#extras-list");

extrasAddBtn.onclick = () => extrasInput.click();
extrasInput.addEventListener("change", e => addExtras([...e.target.files]));

// ── Inline URL dialog (replaces browser prompt() which some browsers block) ──
const urlDialog  = $("#url-dialog");
const urlInput   = $("#url-input");
const urlNameEl  = $("#url-name");
const urlError   = $("#url-error");

function showUrlError(msg) {
  if (!msg) { urlError.classList.add("hidden"); urlError.textContent = ""; return; }
  urlError.textContent = msg; urlError.classList.remove("hidden");
}
function openUrlDialog() {
  urlDialog.classList.remove("hidden");
  urlInput.value = ""; urlNameEl.value = ""; showUrlError("");
  setTimeout(() => urlInput.focus(), 0);
}
function closeUrlDialog() { urlDialog.classList.add("hidden"); showUrlError(""); }

$("#extras-add-url").onclick = openUrlDialog;
$("#url-cancel").onclick      = closeUrlDialog;

urlInput.addEventListener("input", () => {
  // Auto-fill the filename field based on the URL the first time the user types.
  if (urlNameEl.value) return;
  try {
    const u = new URL(urlInput.value);
    const last = decodeURIComponent(u.pathname.split("/").filter(Boolean).pop() || "");
    if (last) urlNameEl.value = last;
  } catch (_) { /* incomplete URL, ignore */ }
});

$("#url-confirm").onclick = () => {
  const url  = urlInput.value.trim();
  const name = urlNameEl.value.trim();
  if (!url)  return showUrlError("URL is required.");
  if (!/^https:\/\//i.test(url)) return showUrlError("Only https:// URLs are accepted.");
  if (!name) return showUrlError("Destination filename is required.");
  if (/^\/|\.\.|\\/.test(name)) return showUrlError("Filename cannot start with /, contain .. or backslashes.");
  if (state.extras.some(x => x.name === name)) return showUrlError(`An entry named "${name}" already exists.`);
  state.extras.push({ name, url, kind: "url" });
  renderExtras();
  closeUrlDialog();
};

[urlInput, urlNameEl].forEach(el =>
  el.addEventListener("keydown", e => {
    if (e.key === "Enter")  { e.preventDefault(); $("#url-confirm").click(); }
    if (e.key === "Escape") { e.preventDefault(); closeUrlDialog(); }
  })
);
["dragenter","dragover"].forEach(ev => extrasDropzone.addEventListener(ev, e => { e.preventDefault(); extrasDropzone.classList.add("border-neon-cyan/60"); }));
["dragleave","drop" ].forEach(ev => extrasDropzone.addEventListener(ev, e => { e.preventDefault(); extrasDropzone.classList.remove("border-neon-cyan/60"); }));
extrasDropzone.addEventListener("drop", e => addExtras([...e.dataTransfer.files]));
extrasDropzone.addEventListener("click", () => extrasInput.click());

async function addExtras(files) {
  for (const f of files) {
    if (state.extras.some(x => x.name === f.name)) continue; // dedup
    const buf = await f.arrayBuffer();
    const b64 = arrayBufferToBase64(buf);
    state.extras.push({ name: f.name, size: f.size, content_b64: b64, kind: "inline" });
  }
  renderExtras();
}

function removeExtra(name) {
  state.extras = state.extras.filter(x => x.name !== name);
  renderExtras();
}

function extrasTotalSize() {
  // Only inline (base64) entries count against the repository_dispatch limit.
  // URL entries are fetched server-side, so their size is irrelevant here.
  return state.extras
    .filter(x => x.kind === "inline")
    .reduce((s, x) => s + (x.content_b64.length + x.name.length + 32), 0);
}

function renderExtras() {
  extrasList.innerHTML = "";
  state.extras.forEach(x => {
    const el = document.createElement("span");
    el.className = "inline-flex items-center gap-2 px-2.5 py-1 rounded-lg bg-ink-800 border border-white/10 text-xs";
    const badge = x.kind === "url"
      ? `<span class="tag-pill">URL</span>`
      : `<span class="text-zinc-500">${(x.size/1024).toFixed(1)} KB</span>`;
    const meta = x.kind === "url"
      ? `<span class="text-zinc-600 max-w-[14rem] truncate" title="${escapeHTML(x.url)}">${escapeHTML(x.url)}</span>`
      : "";
    el.innerHTML = `
      ${badge}
      <span class="text-zinc-300 font-mono">${escapeHTML(x.name)}</span>
      ${meta}
      <button class="text-zinc-500 hover:text-rose-400 ml-1" title="Remove">✕</button>`;
    el.querySelector("button").onclick = () => removeExtra(x.name);
    extrasList.appendChild(el);
  });
  if (state.extras.length === 0) return;
  const total = extrasTotalSize();
  const meta = document.createElement("span");
  const over = total > EXTRAS_MAX_BYTES;
  meta.className = "ml-auto text-xs " + (over ? "text-rose-400" : "text-zinc-500");
  const urlN  = state.extras.filter(x => x.kind === "url").length;
  const inN   = state.extras.length - urlN;
  meta.textContent =
    `${inN} inline · ${urlN} URL · ~${(total/1024).toFixed(1)} KB inline payload${over ? " — over GitHub limit!" : ""}`;
  extrasList.appendChild(meta);
}

function arrayBufferToBase64(buf) {
  const bytes = new Uint8Array(buf);
  let bin = "";
  for (let i = 0; i < bytes.length; i++) bin += String.fromCharCode(bytes[i]);
  return btoa(bin);
}

$("#btn-clear").onclick  = () => { editor.value=""; dzMeta.textContent=""; $("#result").classList.add("hidden"); };
$("#btn-copy").onclick   = () => navigator.clipboard.writeText(editor.value);
$("#btn-format").onclick = async () => {
  try {
    const Y = await ensureYAML();
    const doc = Y.load(editor.value || "", { schema: ESPHOME_SCHEMA });
    editor.value = Y.dump(doc, { lineWidth: 120, noRefs: true, schema: ESPHOME_SCHEMA });
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
  try { doc = Y.load(text, { schema: ESPHOME_SCHEMA }); } catch (e) { err = e; }

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
    editor.value = BOARD_TEMPLATE(b, {
      esphomeBranch: state.branch,
      repoSlug: state.repo && state.repo.full_name,
    });
    validate();
  }
  renderBoards();
}

chipSel.addEventListener("change", renderBoards);
searchInp.addEventListener("input", debounce(renderBoards, 120));
renderBoards();

// ──────────────────────────────────────────────────────────────
// GitHub repo picker (public repos, no token required)
const repoGrid    = $("#repo-grid");
const repoStatus  = $("#repo-status");
const repoOwner   = $("#repo-owner");
const repoSearch  = $("#repo-search");
const repoReload  = $("#repo-reload");
const repoOwnerLabel = $("#repo-owner-label");

let repoCache = []; // last loaded list

async function loadRepos() {
  const owner = repoOwner.value.trim() || "youkorr";
  repoOwnerLabel.textContent = owner;
  repoStatus.textContent = `Loading repos for ${owner}…`;
  repoGrid.innerHTML = "";
  repoCache = [];

  // Try user first, fall back to org. Public, no auth.
  const urls = [
    `https://api.github.com/users/${owner}/repos?per_page=100&sort=updated`,
    `https://api.github.com/orgs/${owner}/repos?per_page=100&sort=updated`,
  ];
  let data = null, lastErr = null, lastStatus = null, rateRemaining = null, rateReset = null;
  for (const u of urls) {
    try {
      console.log("[repos] fetching", u);
      const r = await fetch(u);
      lastStatus    = r.status;
      rateRemaining = r.headers.get("x-ratelimit-remaining");
      rateReset     = r.headers.get("x-ratelimit-reset");
      if (r.ok) { data = await r.json(); break; }
      const body = await r.text();
      console.warn("[repos] non-ok", r.status, body.slice(0, 200));
      lastErr = `HTTP ${r.status}`;
      if (r.status === 403) lastErr += " (rate limit?)";
      if (r.status === 404) lastErr += " (not found)";
    } catch (e) {
      console.error("[repos] fetch error", e);
      lastErr = e.message || "network error";
    }
  }
  if (!data) {
    let extra = "";
    if (lastStatus === 403 && rateRemaining === "0" && rateReset) {
      const reset = new Date(parseInt(rateReset, 10) * 1000);
      extra = ` — GitHub anonymous rate limit reached, resets at ${reset.toLocaleTimeString()}.`;
    }
    repoStatus.innerHTML =
      `<span class="text-rose-400">Failed to load repos for "<b>${escapeHTML(owner)}</b>": ${escapeHTML(lastErr || "unknown")}${escapeHTML(extra)}</span>`;
    return;
  }

  repoCache = data
    .filter(r => !r.fork && !r.archived)
    .sort((a,b) => new Date(b.updated_at) - new Date(a.updated_at));

  if (repoCache.length === 0) {
    repoStatus.innerHTML = `<span class="text-amber-300">No public repos found for "<b>${escapeHTML(owner)}</b>".</span>`;
    return;
  }

  repoStatus.innerHTML = `<span class="text-zinc-400">${repoCache.length} repos loaded · click one to use it as <code class="font-mono">external_components</code> source.</span>`;
  renderRepos();
}

function renderRepos() {
  const q = repoSearch.value.toLowerCase().trim();
  repoGrid.innerHTML = "";
  const list = repoCache.filter(r => !q || (r.name + " " + (r.description||"")).toLowerCase().includes(q));
  list.forEach(r => {
    const el = document.createElement("div");
    el.className = "board-card";
    if (state.repo && state.repo.full_name === r.full_name) el.classList.add("selected");
    const updated = new Date(r.updated_at);
    const ago = timeAgo(updated);
    el.innerHTML = `
      <div class="flex items-start justify-between gap-2">
        <div class="min-w-0">
          <div class="text-[11px] uppercase tracking-wider text-zinc-500">${escapeHTML(r.owner.login)}</div>
          <div class="font-semibold mt-0.5 leading-tight truncate">${escapeHTML(r.name)}</div>
        </div>
        ${r.private ? `<span class="tag-pill">private</span>` : ""}
      </div>
      <div class="text-xs text-zinc-400 mt-2 line-clamp-2 min-h-[2.4em]">${escapeHTML(r.description || "—")}</div>
      <div class="mt-3 flex flex-wrap gap-1.5 items-center">
        ${r.language ? `<span class="chip-pill">${escapeHTML(r.language)}</span>` : ""}
        <span class="chip-pill">★ ${r.stargazers_count}</span>
        <span class="chip-pill">⤴ ${ago}</span>
      </div>`;
    el.onclick = () => selectRepo(r);
    repoGrid.appendChild(el);
  });
  if (list.length === 0) repoGrid.innerHTML = `<div class="col-span-full text-sm text-zinc-500">No repo matches "${escapeHTML(q)}".</div>`;
}

function selectRepo(r) {
  state.repo = r;
  $("#sel-repo").textContent = `github://${r.full_name}`;
  // Regenerate template if editor is empty OR if it was an auto-generated template
  if (!editor.value.trim() || editor.value.startsWith("# ─── Generated by ncaseonetwo.xyz ───")) {
    if (state.board) {
      editor.value = BOARD_TEMPLATE(state.board, {
        esphomeBranch: state.branch,
        repoSlug: r.full_name,
      });
      validate();
    }
  }
  renderRepos();
}

function timeAgo(d) {
  const sec = Math.floor((Date.now() - d.getTime()) / 1000);
  if (sec < 60)        return `${sec}s`;
  if (sec < 3600)      return `${Math.floor(sec/60)}m`;
  if (sec < 86400)     return `${Math.floor(sec/3600)}h`;
  if (sec < 86400*30)  return `${Math.floor(sec/86400)}d`;
  if (sec < 86400*365) return `${Math.floor(sec/86400/30)}mo`;
  return `${Math.floor(sec/86400/365)}y`;
}

repoSearch.addEventListener("input", debounce(renderRepos, 120));
repoReload.addEventListener("click", loadRepos);
repoOwner.addEventListener("change", loadRepos);
loadRepos();

// ──────────────────────────────────────────────────────────────
// Branch toggle
$$(".branch-btn").forEach(b => b.addEventListener("click", () => {
  state.branch = b.dataset.branch;
  $$(".branch-btn").forEach(x => { x.classList.remove("active","bg-white/10"); x.classList.add("text-zinc-400"); });
  b.classList.add("active");
  b.classList.remove("text-zinc-400");
}));
$('.branch-btn[data-branch="stable"]').classList.add("active");

// Runner toggle
$$(".runner-btn").forEach(b => b.addEventListener("click", () => {
  state.runner = b.dataset.runner;
  $$(".runner-btn").forEach(x => { x.classList.remove("active","bg-white/10"); x.classList.add("text-zinc-400"); });
  b.classList.add("active");
  b.classList.remove("text-zinc-400");
}));
$('.runner-btn[data-runner="self"]').classList.add("active");

// ──────────────────────────────────────────────────────────────
// Compile
const log = $("#log");
const statusDot = $("#status-dot");
const statusLabel = $("#status-label");
const runLink = $("#run-link");
const btnStop = $("#btn-stop");

// run_id currently being polled — null when idle / completed.
let currentRunId = null;

function setStatus(s, label) {
  statusDot.className = "w-2 h-2 rounded-full bg-zinc-600";
  if (s === "running") statusDot.classList.add("status-running");
  if (s === "ok")      statusDot.classList.add("status-ok");
  if (s === "err")     statusDot.classList.add("status-err");
  statusLabel.textContent = label;
  // Stop button only makes sense while a run is in flight.
  if (btnStop) {
    if (s === "running") btnStop.classList.remove("hidden");
    else                 btnStop.classList.add("hidden");
  }
}

// ──────────────────────────────────────────────────────────────
// SVG device mockup — paints the on-screen content based on a state.
//
//   state ∈ { "idle", "compiling", "success", "error", "cancelled" }
//   info  — optional context: { board, chip, message, progress }
const mockupContent = () => document.getElementById("mockup-content");
const mockupScreen  = () => document.getElementById("mockup-screen");
const mockupCap     = () => document.getElementById("mockup-caption");

function setMockupState(s, info = {}) {
  const c = mockupContent();
  const screen = mockupScreen();
  if (!c || !screen) return;
  const board = info.board || (state.board && state.board.name) || "ncaseonetwo";

  let svg = "";
  let cap = "";
  let screenFill = "url(#screen-off)";

  if (s === "compiling") {
    screenFill = "#0a1620";
    const pct = Math.max(0, Math.min(100, info.progress || 0));
    svg = `
      <text x="200" y="120" fill="#00f5d4" font-size="20" font-weight="600">building…</text>
      <text x="200" y="148" fill="#71717a" font-size="13">${escapeHTML(board)}</text>
      <rect x="80" y="180" width="240" height="6" rx="3" fill="#1f2937"/>
      <rect x="80" y="180" width="${(240 * pct / 100).toFixed(1)}" height="6" rx="3" fill="#00f5d4">
        <animate attributeName="opacity" values="0.6;1;0.6" dur="1.4s" repeatCount="indefinite"/>
      </rect>
      <text x="200" y="210" fill="#52525b" font-size="11">${pct}%</text>`;
    cap = `Compiling on ${info.runner || "—"}…`;
  } else if (s === "success") {
    screenFill = "#062017";
    svg = `
      <circle cx="200" cy="120" r="32" fill="none" stroke="#84cc16" stroke-width="3"/>
      <path d="M184 122 l12 12 l22 -26" fill="none" stroke="#84cc16" stroke-width="4" stroke-linecap="round" stroke-linejoin="round"/>
      <text x="200" y="190" fill="#a3e635" font-size="18" font-weight="600">ready to flash</text>
      <text x="200" y="214" fill="#71717a" font-size="12">${escapeHTML(board)}</text>`;
    cap = "Firmware built — download or flash it.";
  } else if (s === "error" || s === "failure") {
    screenFill = "#1c0710";
    svg = `
      <circle cx="200" cy="120" r="32" fill="none" stroke="#f43f5e" stroke-width="3"/>
      <path d="M186 106 l28 28 M214 106 l-28 28" stroke="#f43f5e" stroke-width="4" stroke-linecap="round"/>
      <text x="200" y="190" fill="#fb7185" font-size="18" font-weight="600">build failed</text>
      <text x="200" y="214" fill="#71717a" font-size="11">${escapeHTML(info.message || "see log above")}</text>`;
    cap = "Something went wrong — open the run for details.";
  } else if (s === "cancelled") {
    screenFill = "#13141a";
    svg = `
      <circle cx="200" cy="120" r="32" fill="none" stroke="#a1a1aa" stroke-width="3"/>
      <path d="M180 100 l40 40" stroke="#a1a1aa" stroke-width="3" stroke-linecap="round"/>
      <text x="200" y="190" fill="#d4d4d8" font-size="18" font-weight="600">cancelled</text>`;
    cap = "Build cancelled.";
  } else {
    // idle
    svg = `
      <text x="200" y="140" fill="#3f3f46" font-size="14">screen off</text>
      <circle cx="200" cy="170" r="3" fill="#27272a"/>`;
    cap = "Idle — pick a board and hit Compile.";
  }

  screen.setAttribute("fill", screenFill);
  c.innerHTML = svg;
  mockupCap().textContent = cap;

  // Power LED reflects the lifecycle: amber while running, lime on success,
  // rose on failure. Mirrors the status dot in the build-progress header so
  // the device "feels" alive even when the screen is off-state.
  const led = document.getElementById("mockup-led");
  if (led) {
    const c = s === "compiling" ? "#f59e0b"
            : s === "success"   ? "#84cc16"
            : s === "error" || s === "failure" ? "#f43f5e"
            : s === "cancelled" ? "#71717a"
            : "#27272a";
    led.setAttribute("fill", c);
  }
}

// Stop button → ask Worker to cancel the current run.
if (btnStop) {
  btnStop.onclick = async () => {
    if (!currentRunId) return;
    btnStop.disabled = true;
    appendLog(`■ cancelling run ${currentRunId}…`);
    try {
      const r = await fetch(`${CONFIG.apiBase}/cancel?run_id=${currentRunId}`, { method: "POST" });
      if (!r.ok) throw new Error(`HTTP ${r.status}: ${await r.text()}`);
      // The poll loop will see the run transition to "cancelled" and update
      // status / mockup itself. We don't tear anything down here.
    } catch (e) {
      appendLog("✖ cancel failed: " + e.message);
      btnStop.disabled = false;
    }
  };
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
  setMockupState("compiling", { progress: 5, runner: state.runner === "self" ? "Unraid" : "GitHub" });
  appendLog(`▶ board    : ${state.board.brand} ${state.board.name} (${state.board.id})`);
  appendLog(`▶ chip     : ${CHIP_LABEL[state.board.chip] || state.board.chip}`);
  appendLog(`▶ esphome  : ${state.branch}`);
  appendLog(`▶ runner   : ${state.runner === "self" ? "self-hosted (Unraid)" : "GitHub-hosted (ubuntu-latest)"}`);
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
    if (extrasTotalSize() > EXTRAS_MAX_BYTES) {
      appendLog("✖ Extra files payload too large for GitHub repository_dispatch (60 KB limit).");
      setStatus("err", "payload too large"); return;
    }
    if (state.extras.length) appendLog(`▶ extras   : ${state.extras.map(x => x.name).join(", ")}`);

    const r = await fetch(CONFIG.apiBase + "/compile", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({
        yaml: editor.value,
        board: state.board.id,
        branch: state.branch,
        runner: state.runner,
        files: state.extras.map(x => x.kind === "url"
          ? ({ name: x.name, url: x.url })
          : ({ name: x.name, content_b64: x.content_b64 })),
      }),
    });
    if (!r.ok) throw new Error(`HTTP ${r.status}: ${await r.text()}`);
    const { run_id, html_url } = await r.json();
    currentRunId = run_id;
    appendLog(`✓ dispatched · run_id=${run_id}`);
    runLink.href = html_url; runLink.classList.remove("hidden");
    pollStatus(run_id);
  } catch (e) {
    appendLog("✖ " + e.message);
    setStatus("err", "failed");
    setMockupState("error", { message: e.message });
  }
};

async function pollStatus(runId) {
  let dots = 0;
  while (true) {
    await sleep(3000);
    let s;
    try {
      const r = await fetch(`${CONFIG.apiBase}/status?run_id=${runId}`);
      s = await r.json();
    } catch (e) { appendLog("… retry"); continue; }

    renderSteps(s.jobs || []);

    // Surface which runner picked up the job, if any.
    const job = (s.jobs || [])[0];
    if (job && job.runner_name) {
      const ri = $("#runner-info");
      ri.classList.remove("hidden");
      ri.textContent = `runner: ${job.runner_name}`;
    }

    setStatus("running", `${s.status}${".".repeat(++dots % 4)}`);

    // Rough progress estimate from the proportion of completed steps so the
    // mockup's progress bar moves visibly while the build runs.
    if (s.status !== "completed") {
      const all = (s.jobs || []).flatMap(j => j.steps || []);
      const done = all.filter(st => st.status === "completed").length;
      const pct  = all.length ? Math.round((done / all.length) * 100) : 0;
      setMockupState("compiling", {
        progress: Math.max(5, pct),
        runner: job && job.runner_name ? job.runner_name : (state.runner === "self" ? "Unraid" : "GitHub"),
      });
    }

    if (s.status === "completed") {
      currentRunId = null;
      if (s.conclusion === "success") {
        setStatus("ok", "success");
        setMockupState("success");
        appendLog("");
        appendLog("✓ build successful");
        (s.artifacts || []).forEach(a => appendLog(`  ↓ ${a.name}: ${a.url}`));
      } else if (s.conclusion === "cancelled") {
        setStatus("err", "cancelled");
        setMockupState("cancelled");
        appendLog("■ build cancelled");
      } else {
        setStatus("err", s.conclusion || "failed");
        setMockupState("error", { message: `build ${s.conclusion || "failed"}` });
        appendLog("✖ build " + s.conclusion);
      }
      return;
    }
  }
}

// Paint the initial idle state. The script tag lives at the end of <body>
// so the SVG nodes are already parsed by the time we get here.
setMockupState("idle");

// ──────────────────────────────────────────────────────────────
// Step timeline rendering
const stepsEl = $("#steps");

function renderSteps(jobs) {
  if (!jobs.length) {
    stepsEl.innerHTML = `<div class="text-zinc-500 text-xs">Waiting for runner to pick up the job…</div>`;
    return;
  }
  let html = "";
  for (const job of jobs) {
    for (const step of (job.steps || [])) {
      const icon = stepIcon(step);
      const dur  = stepDuration(step);
      const cls  = stepRowClass(step);
      html += `
        <div class="flex items-center gap-3 py-1.5 ${cls}">
          <span class="w-5 h-5 grid place-items-center shrink-0">${icon}</span>
          <span class="flex-1 truncate ${step.status === 'in_progress' ? 'text-zinc-100' : 'text-zinc-300'}">${escapeHTML(step.name)}</span>
          ${dur ? `<span class="text-[11px] font-mono text-zinc-500 shrink-0">${dur}</span>` : ""}
        </div>`;
    }
  }
  stepsEl.innerHTML = html;
  // Auto-scroll so the running step stays in view.
  const running = stepsEl.querySelector(".step-running");
  if (running) running.scrollIntoView({ block: "nearest", behavior: "smooth" });
}

function stepIcon(step) {
  if (step.status === "in_progress") {
    return `<svg class="animate-spin text-neon-cyan" width="14" height="14" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="3" stroke-linecap="round">
      <path d="M12 2a10 10 0 0 1 10 10" /></svg>`;
  }
  if (step.status !== "completed") {
    return `<span class="w-2 h-2 rounded-full bg-zinc-600 block"></span>`;
  }
  if (step.conclusion === "success") return `<span class="text-lime-400">✓</span>`;
  if (step.conclusion === "failure") return `<span class="text-rose-400">✕</span>`;
  if (step.conclusion === "skipped") return `<span class="text-zinc-600">⊘</span>`;
  if (step.conclusion === "cancelled") return `<span class="text-zinc-500">⊗</span>`;
  return `<span class="text-zinc-500">·</span>`;
}

function stepRowClass(step) {
  if (step.status === "in_progress")            return "step-running bg-neon-cyan/5 -mx-2 px-2 rounded-md";
  if (step.conclusion === "failure")            return "bg-rose-500/5 -mx-2 px-2 rounded-md";
  return "";
}

function stepDuration(step) {
  if (!step.started_at) return "";
  const start = new Date(step.started_at).getTime();
  const end   = step.completed_at ? new Date(step.completed_at).getTime() : Date.now();
  const sec   = Math.max(0, Math.round((end - start) / 1000));
  if (sec < 60)  return `${sec}s`;
  const m = Math.floor(sec / 60), s = sec % 60;
  return `${m}m${s.toString().padStart(2,"0")}s`;
}

// ──────────────────────────────────────────────────────────────
// utils
function debounce(fn, ms){ let t; return (...a)=>{ clearTimeout(t); t=setTimeout(()=>fn(...a), ms); }; }
function sleep(ms){ return new Promise(r => setTimeout(r, ms)); }

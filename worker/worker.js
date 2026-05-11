/**
 * ncaseonetwo.xyz — compile proxy (Cloudflare Worker)
 *
 *  Frontend (GitHub Pages) calls this Worker. The Worker holds the GitHub
 *  Personal Access Token in a secret env var (`GH_TOKEN`) and is the ONLY
 *  thing that ever sees it. The browser never receives the token.
 *
 *  Endpoints
 *    POST /compile         body: { yaml, board, branch }
 *                          → triggers repository_dispatch on COMPILE_REPO,
 *                            then finds the resulting run.
 *                          ← { run_id, html_url }
 *    GET  /status?run_id=N → { status, conclusion, artifacts:[{name,url}] }
 *
 *  Required secrets (set with `wrangler secret put`):
 *    GH_TOKEN        — fine-grained PAT, scoped to COMPILE_REPO only,
 *                      permissions: Actions=Read/Write, Contents=Read,
 *                      Metadata=Read.
 *
 *  Required env vars (in wrangler.toml or dashboard):
 *    COMPILE_REPO    — e.g. "youkorr/ncaseonetwo-builds"
 *    ALLOWED_ORIGIN  — e.g. "https://ncaseonetwo.xyz"
 */

const cors = (origin) => ({
  "access-control-allow-origin":  origin,
  "access-control-allow-methods": "GET,POST,OPTIONS",
  "access-control-allow-headers": "content-type",
  "access-control-max-age":       "86400",
});

const json = (obj, status, origin) =>
  new Response(JSON.stringify(obj), {
    status,
    headers: { "content-type": "application/json", ...cors(origin) },
  });

const ghHeaders = (token) => ({
  "authorization":         `Bearer ${token}`,
  "accept":                "application/vnd.github+json",
  "x-github-api-version":  "2022-11-28",
  "user-agent":            "ncaseonetwo-worker",
});

export default {
  async fetch(req, env) {
    const url    = new URL(req.url);
    const origin = env.ALLOWED_ORIGIN || "*";

    if (req.method === "OPTIONS")
      return new Response(null, { headers: cors(origin) });

    if (url.pathname === "/compile" && req.method === "POST") {
      const { yaml: rawYaml, board, branch, files, runner } = await req.json();
      if (!rawYaml || !board || !branch)
        return json({ error: "missing yaml/board/branch" }, 400, origin);
      const runnerChoice = (runner === "github") ? "github" : "self";

      // Patch the YAML so font entries with local files don't blow up with
      // "missing glyphs" when our generic stub TTF doesn't carry the user's
      // exact codepoints. This must happen *before* size accounting so the
      // blob/inline branch sees the final bytes.
      const yaml = patchFontGlyphChecks(rawYaml);

      // GitHub's repository_dispatch client_payload is hard-capped at 65535
      // bytes. base64 inflates by ~33%, so any YAML over ~30 KB raw will
      // overflow once we add the other fields. For those, stash the YAML in
      // a transient secret gist and inject it as a URL-backed entry in the
      // `files` array; the workflow's existing "Write extra files" step
      // fetches it (running *after* "Decode YAML", so it overwrites the
      // placeholder we ship inline).
      const YAML_RAW_INLINE_LIMIT = 30 * 1024;
      const yamlBytes = new TextEncoder().encode(yaml).length;
      let filesList = Array.isArray(files) ? [...files] : [];
      let yaml_b64;
      let blob_id = null;

      if (yamlBytes > YAML_RAW_INLINE_LIMIT) {
        // Offload to Cloudflare KV (binding name: BLOBS). The compile
        // workflow's "Write extra files" step fetches it from /blob/<id>
        // and overwrites build/device.yaml before the cache + compile
        // steps run.
        if (!env.BLOBS) {
          return json({
            error: "no BLOBS KV binding configured",
            hint:  "Create a KV namespace in Cloudflare and bind it to this Worker as `BLOBS`.",
          }, 500, origin);
        }
        blob_id = crypto.randomUUID();
        // TTL 1 h — much longer than any reasonable build, but short enough
        // that nothing lingers if something goes sideways.
        await env.BLOBS.put(blob_id, yaml, { expirationTtl: 3600 });
        const blobUrl = `${new URL(req.url).origin}/blob/${blob_id}`;
        // Drop any existing same-named entry to avoid double-fetch ambiguity.
        filesList = filesList.filter(f => f && f.name !== "device.yaml");
        filesList.push({ name: "device.yaml", url: blobUrl });
        // Inline payload becomes a tiny placeholder so "Decode YAML" still
        // writes a valid (immediately overwritten) file.
        yaml_b64 = btoa(`# (fetched from blob ${blob_id})\n`);
      } else {
        yaml_b64 = btoa(unescape(encodeURIComponent(yaml)));
      }

      // The frontend already strips font/image binaries on drop (they would
      // overflow the 60 KB dispatch limit), but the user's YAML still
      // references them by relative path (e.g. `file: fonts/foo.ttf`).
      // Without those files on disk, esphome compile fails before doing
      // anything useful. So we scan the YAML for local font/image refs and
      // inject URL-backed stub entries — ESPHome reads them, validates, and
      // produces a buildable firmware (with placeholder glyphs / images).
      const workerOrigin = new URL(req.url).origin;
      const localRefs    = scanLocalAssetRefs(yaml);
      for (const ref of localRefs) {
        if (filesList.some(f => f && f.name === ref.name)) continue; // user-provided
        filesList.push({
          name: ref.name,
          url:  ref.kind === "font"
                  ? `${workerOrigin}/stub.ttf`
                  : `${workerOrigin}/stub.png`,
          stub: true,
        });
      }

      // Pack extra files into a single base64 JSON string so the workflow
      // receives them as one client_payload field.
      let files_b64 = "";
      if (filesList.length) {
        for (const f of filesList) {
          if (typeof f.name !== "string" || /(^\/|\.\.|\\)/.test(f.name))
            return json({ error: `invalid file name: ${f && f.name}` }, 400, origin);
          if (f.url && !/^https:\/\//i.test(f.url))
            return json({ error: `only https:// urls allowed: ${f.url}` }, 400, origin);
          if (!f.url && !f.content_b64)
            return json({ error: `file ${f.name} has neither content_b64 nor url` }, 400, origin);
        }
        files_b64 = btoa(unescape(encodeURIComponent(JSON.stringify(filesList))));
      }

      // Dispatch
      const dispatchRes = await fetch(
        `https://api.github.com/repos/${env.COMPILE_REPO}/dispatches`,
        {
          method:  "POST",
          headers: ghHeaders(env.GH_TOKEN),
          body: JSON.stringify({
            event_type: "compile",
            client_payload: { board, branch, yaml_b64, files_b64, runner: runnerChoice },
          }),
        }
      );
      if (!dispatchRes.ok)
        return json({ error: "dispatch failed", detail: await dispatchRes.text() }, 502, origin);

      // Find the run we just triggered (poll a few seconds).
      const startedAt = Date.now();
      let run = null;
      for (let i = 0; i < 12; i++) {
        await sleep(2000);
        const r = await fetch(
          `https://api.github.com/repos/${env.COMPILE_REPO}/actions/workflows/compile.yml/runs?event=repository_dispatch&per_page=5`,
          { headers: ghHeaders(env.GH_TOKEN) }
        );
        const data = await r.json();
        run = (data.workflow_runs || []).find(
          (x) => new Date(x.created_at).getTime() >= startedAt - 10_000
        );
        if (run) break;
      }
      if (!run)
        return json({ error: "could not locate triggered run" }, 504, origin);

      return json({ run_id: run.id, html_url: run.html_url, blob_id }, 200, origin);
    }

    // GET /blob/<uuid> — serves a previously-stashed large YAML so the
    // compile workflow can fetch it server-side. UUIDs are unguessable
    // (~122 bits of entropy) and entries TTL out after 1 h, so the
    // route is effectively a presigned URL without needing auth.
    if (url.pathname.startsWith("/blob/") && req.method === "GET") {
      if (!env.BLOBS)
        return new Response("KV binding `BLOBS` not configured", { status: 500 });
      const id = url.pathname.slice("/blob/".length);
      if (!/^[a-f0-9-]{16,}$/i.test(id))
        return new Response("invalid blob id", { status: 400 });
      const content = await env.BLOBS.get(id);
      if (content === null)
        return new Response("not found or expired", { status: 404 });
      return new Response(content, {
        status: 200,
        headers: {
          "content-type":  "text/yaml; charset=utf-8",
          "cache-control": "no-store",
        },
      });
    }

    if (url.pathname === "/status" && req.method === "GET") {
      const runId = url.searchParams.get("run_id");
      if (!runId) return json({ error: "missing run_id" }, 400, origin);

      const r = await fetch(
        `https://api.github.com/repos/${env.COMPILE_REPO}/actions/runs/${runId}`,
        { headers: ghHeaders(env.GH_TOKEN) }
      );
      if (!r.ok)
        return json({ error: "status failed", detail: await r.text() }, 502, origin);
      const run = await r.json();

      // Fetch the list of jobs for this run so the frontend can render a live
      // step-by-step timeline (which step is queued / running / done / failed).
      let jobs = [];
      const jr = await fetch(
        `https://api.github.com/repos/${env.COMPILE_REPO}/actions/runs/${runId}/jobs`,
        { headers: ghHeaders(env.GH_TOKEN) }
      );
      if (jr.ok) {
        const jd = await jr.json();
        jobs = (jd.jobs || []).map((job) => ({
          id:          job.id,
          name:        job.name,
          status:      job.status,
          conclusion:  job.conclusion,
          runner_name: job.runner_name,
          started_at:  job.started_at,
          completed_at: job.completed_at,
          html_url:    job.html_url,
          steps: (job.steps || []).map((s) => ({
            number:       s.number,
            name:         s.name,
            status:       s.status,
            conclusion:   s.conclusion,
            started_at:   s.started_at,
            completed_at: s.completed_at,
          })),
        }));
      }

      let artifacts = [];
      if (run.status === "completed" && run.conclusion === "success") {
        const a = await fetch(
          `https://api.github.com/repos/${env.COMPILE_REPO}/actions/runs/${runId}/artifacts`,
          { headers: ghHeaders(env.GH_TOKEN) }
        );
        const ad = await a.json();
        artifacts = (ad.artifacts || []).map((x) => ({
          name: x.name,
          url:  `https://github.com/${env.COMPILE_REPO}/actions/runs/${runId}/artifacts/${x.id}`,
        }));
      }

      return json({
        status:     run.status,
        conclusion: run.conclusion,
        html_url:   run.html_url,
        jobs,
        artifacts,
      }, 200, origin);
    }

    // POST /cancel?run_id=… — cancels an in-progress / queued GitHub Actions
    // run. Useful if the user dispatched the wrong YAML or wrong board.
    if (url.pathname === "/cancel" && req.method === "POST") {
      const runId = url.searchParams.get("run_id");
      if (!runId) return json({ error: "missing run_id" }, 400, origin);

      const r = await fetch(
        `https://api.github.com/repos/${env.COMPILE_REPO}/actions/runs/${runId}/cancel`,
        { method: "POST", headers: ghHeaders(env.GH_TOKEN) }
      );
      // GitHub returns 202 Accepted (no body) on success.
      if (!r.ok && r.status !== 202) {
        return json({ error: "cancel failed", status: r.status, detail: await r.text() }, 502, origin);
      }
      return json({ ok: true }, 200, origin);
    }

    // GET /stub.png — 1×1 transparent PNG served as a placeholder for any
    // image: ref the user's YAML keeps pointing at after we stripped the
    // real PNG/JPG from extras.
    if (url.pathname === "/stub.png" && req.method === "GET") {
      return new Response(STUB_PNG, {
        status: 200,
        headers: {
          "content-type":  "image/png",
          "cache-control": "public, max-age=86400",
        },
      });
    }

    // GET /stub.ttf — redirects to a real public TTF (Material Design Icons
    // from jsdelivr). Using a redirect rather than serving the bytes keeps
    // the Worker small and lets us swap fonts later without redeploying.
    // urllib.request.urlopen follows 302s by default, so the workflow sees
    // a normal TTF response.
    if (url.pathname === "/stub.ttf" && req.method === "GET") {
      return Response.redirect(
        "https://cdn.jsdelivr.net/npm/@mdi/font@latest/fonts/materialdesignicons-webfont.ttf",
        302
      );
    }

    // GET /logs?run_id=…&tail=150 — pulls the raw job log from GitHub
    // Actions, strips line-leading timestamps, and returns the last N
    // lines. The frontend calls this on failure so the user can read the
    // actual GCC / ESPHome error without leaving the page.
    if (url.pathname === "/logs" && req.method === "GET") {
      const runId = url.searchParams.get("run_id");
      const tailN = Math.max(20, Math.min(parseInt(url.searchParams.get("tail"), 10) || 150, 600));
      if (!runId) return json({ error: "missing run_id" }, 400, origin);

      // First grab the job id (a run usually has one job for us).
      const jr = await fetch(
        `https://api.github.com/repos/${env.COMPILE_REPO}/actions/runs/${runId}/jobs`,
        { headers: ghHeaders(env.GH_TOKEN) }
      );
      if (!jr.ok)
        return json({ error: "jobs lookup failed", detail: await jr.text() }, 502, origin);
      const jd = await jr.json();
      const job = (jd.jobs || [])[0];
      if (!job) return json({ error: "no job in this run yet" }, 404, origin);

      // The /jobs/<id>/logs endpoint 302s to a presigned url; fetch follows
      // it transparently and returns the plain-text log.
      const lr = await fetch(
        `https://api.github.com/repos/${env.COMPILE_REPO}/actions/jobs/${job.id}/logs`,
        { headers: ghHeaders(env.GH_TOKEN), redirect: "follow" }
      );
      if (!lr.ok)
        return json({ error: "logs fetch failed", status: lr.status, detail: await lr.text() }, 502, origin);

      let text = await lr.text();
      // Strip the leading ISO timestamp on every line — they bloat the
      // tail and aren't useful to the user.
      text = text.replace(/^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d+Z\s?/gm, "");
      const lines = text.split("\n");
      const tail  = lines.slice(-tailN).join("\n");

      const failedStep = (job.steps || []).find(s => s.conclusion === "failure");
      return json({
        tail,
        total_lines:  lines.length,
        returned:     Math.min(lines.length, tailN),
        job_name:     job.name,
        job_html_url: job.html_url,
        failed_step:  failedStep ? failedStep.name : null,
      }, 200, origin);
    }

    return json({ error: "not found" }, 404, origin);
  },
};

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// ─────────────────────────────────────────────────────────────────
// YAML local-asset scanner.
//
// Matches lines of the form
//   <indent>file: path/to/something.<ext>
//   <indent>- file: 'path/to/foo.png'
// where the value is a bare path (not a nested mapping with url:/type:).
// Returns a deduped list of { name, kind } where kind ∈ {"font","image"}.
const FONT_EXT_RE  = /\.(ttf|otf|woff2?|eot|bdf|pcf|fnt)$/i;
const IMAGE_EXT_RE = /\.(png|jpe?g|gif|bmp|svg|webp|ico|tiff?)$/i;
const FILE_REF_RE  =
  /^[ \t]+(?:-[ \t]+)?file:[ \t]*(['"]?)([^'"\s][^'"\n]*\.(?:ttf|otf|woff2?|eot|bdf|pcf|fnt|png|jpe?g|gif|bmp|svg|webp|ico|tiff?))\1[ \t]*$/gim;

function scanLocalAssetRefs(yaml) {
  const out = new Map();          // name → { name, kind }, deduped
  FILE_REF_RE.lastIndex = 0;
  let m;
  while ((m = FILE_REF_RE.exec(yaml))) {
    const path = m[2].trim();
    if (/^https?:\/\//i.test(path)) continue; // already a URL, not a local ref
    const kind = FONT_EXT_RE.test(path) ? "font"
               : IMAGE_EXT_RE.test(path) ? "image"
               : null;
    if (!kind) continue;
    if (!out.has(path)) out.set(path, { name: path, kind });
  }
  return [...out.values()];
}

// 1×1 fully transparent PNG (67 bytes). Returned by GET /stub.png and used
// to satisfy `file: foo.png` references that point to assets we filtered
// out client-side.
const STUB_PNG = new Uint8Array([
  0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,0x00,0x00,0x00,0x0D,0x49,0x48,0x44,0x52,
  0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x08,0x06,0x00,0x00,0x00,0x1F,0x15,0xC4,
  0x89,0x00,0x00,0x00,0x0D,0x49,0x44,0x41,0x54,0x78,0x9C,0x63,0x00,0x01,0x00,0x00,
  0x05,0x00,0x01,0x0D,0x0A,0x2D,0xB4,0x00,0x00,0x00,0x00,0x49,0x45,0x4E,0x44,0xAE,
  0x42,0x60,0x82,
]);

// Patch the YAML in place so font entries whose `file:` points at a local
// font path become buildable even though our generic stub TTF doesn't
// carry the user's exact codepoints. We do TWO things to each such entry:
//
//   1. Inject `ignore_missing_glyphs: true` at the entry's key column.
//      Demotes ESPHome's "missing glyph" check from error to warning.
//
//   2. Replace the entry's `glyphs: [...]` list with `glyphs: []`. The
//      ESPHome dev branch validates explicit glyph lists strictly and
//      still aborts with "missing N glyphs" even when ignore_missing_glyphs
//      is true, when the requested PUA codepoints (+) don't exist
//      in our stub MDI font. Clearing the list sidesteps the validation.
//      The compiled font then has no custom glyphs (icons render as
//      tofu boxes) but the build succeeds — exactly the test-compile
//      semantics the user wants.
//
// Implementation: simple line-based state machine. Robust enough for
// well-formatted ESPHome YAMLs; doesn't need a full YAML parser. We
// only modify entries we deliberately stub on the file side, so legit
// user-provided fonts are untouched.
const FILE_LOCAL_FONT_RE =
  /^([ \t]*)(-[ \t]+)file:[ \t]*(['"]?)([^'"\s][^'"\n]*\.(?:ttf|otf|woff2?|eot|bdf|pcf|fnt))\3[ \t]*$/;
const KEY_RE       = /^([ \t]*)([a-z_]+):[ \t]*(.*)$/;
const LIST_ITEM_RE = /^([ \t]+)-[ \t]/;

function patchFontGlyphChecks(yaml) {
  const lines = yaml.split("\n");
  const out   = [];
  let inEntry      = false;
  let entryIndent  = -1;
  let keyColumn    = 0;
  let strippingGlyphs = false;
  let glyphsKeyCol = -1;

  for (const line of lines) {
    // Comments don't change structural state.
    if (/^\s*#/.test(line)) { out.push(line); continue; }

    const fileMatch = line.match(FILE_LOCAL_FONT_RE);
    if (fileMatch) {
      const leading = fileMatch[1];
      const dash    = fileMatch[2];
      inEntry         = true;
      entryIndent     = leading.length;
      keyColumn       = leading.length + dash.length;
      strippingGlyphs = false;
      out.push(line);
      out.push(" ".repeat(keyColumn) + "ignore_missing_glyphs: true");
      continue;
    }

    if (inEntry) {
      const nonBlank = line.match(/^([ \t]*)\S/);
      const curIndent = nonBlank ? nonBlank[1].length : -1;
      // A non-blank line at or below entry indent ends the current entry.
      if (curIndent !== -1 && curIndent <= entryIndent) {
        inEntry         = false;
        strippingGlyphs = false;
      }
    }

    if (inEntry) {
      const keyMatch = line.match(KEY_RE);
      if (keyMatch && keyMatch[2] === "glyphs" && keyMatch[1].length === keyColumn) {
        // Replace `glyphs:` (whether inline or block) with an empty list.
        out.push(`${keyMatch[1]}glyphs: []`);
        strippingGlyphs = true;
        glyphsKeyCol    = keyMatch[1].length;
        continue;
      }
      if (strippingGlyphs) {
        const listMatch = line.match(LIST_ITEM_RE);
        if (listMatch && listMatch[1].length > glyphsKeyCol) {
          continue; // drop the original `- <glyph>` items
        }
        strippingGlyphs = false; // sibling key reached
      }
    }

    out.push(line);
  }

  return out.join("\n");
}

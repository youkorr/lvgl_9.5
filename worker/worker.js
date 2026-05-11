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
      const { yaml, board, branch, files, runner } = await req.json();
      if (!yaml || !board || !branch)
        return json({ error: "missing yaml/board/branch" }, 400, origin);
      const runnerChoice = (runner === "github") ? "github" : "self";

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

    return json({ error: "not found" }, 404, origin);
  },
};

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

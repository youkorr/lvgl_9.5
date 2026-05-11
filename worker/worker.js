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

      const yaml_b64 = btoa(unescape(encodeURIComponent(yaml)));

      // Pack extra files into a single base64 JSON string so the workflow
      // receives them as one client_payload field.
      let files_b64 = "";
      if (Array.isArray(files) && files.length) {
        for (const f of files) {
          if (typeof f.name !== "string" || /(^\/|\.\.|\\)/.test(f.name))
            return json({ error: `invalid file name: ${f && f.name}` }, 400, origin);
          if (f.url && !/^https:\/\//i.test(f.url))
            return json({ error: `only https:// urls allowed: ${f.url}` }, 400, origin);
          if (!f.url && !f.content_b64)
            return json({ error: `file ${f.name} has neither content_b64 nor url` }, 400, origin);
        }
        files_b64 = btoa(unescape(encodeURIComponent(JSON.stringify(files))));
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

      return json({ run_id: run.id, html_url: run.html_url }, 200, origin);
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
        artifacts,
      }, 200, origin);
    }

    return json({ error: "not found" }, 404, origin);
  },
};

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// Public site + APIs for the paper-trading engine.
//
//   /                 static UI (Workers static assets), no login
//   /ws               live market view (MarketHub DO fan-out)
//   /api/snapshot     latest view as JSON, cached at the edge for 2 s
//   /api/leaderboard  weekly paper P&L, cached 30 s
//   /auth/*           GitHub / Google OAuth (Turnstile first), session cookie
//   /api/account|order|cancel|reset|privacy|delete   signed-in paper accounts
//   /ingest           telemetry uplink from the home machine (HMAC token)
//   /admin, /admin/api/*   owner only, behind Cloudflare Access (fails closed)
//
// PAPER TRADING ONLY. Nothing here can route an order anywhere real.
import * as auth from "./auth.js";

export { MarketHub, Account, Leaderboard, ClientRateLimiter } from "./durable.js";

const JSON_HEADERS = { "content-type": "application/json; charset=utf-8" };
const SECURITY_HEADERS = {
  "x-content-type-options": "nosniff",
  "referrer-policy": "strict-origin-when-cross-origin",
  "x-frame-options": "DENY",
};

function json(body, status = 200, extra = {}) {
  return new Response(JSON.stringify(body), { status, headers: { ...JSON_HEADERS, ...SECURITY_HEADERS, ...extra } });
}
function text(msg, status) {
  return new Response(msg, { status, headers: { "content-type": "text/plain; charset=utf-8", ...SECURITY_HEADERS } });
}

const hub = (env) => env.HUB.get(env.HUB.idFromName("global"));

async function limited(env, key, limit, periodS = 60) {
  const stub = env.LIMITER.get(env.LIMITER.idFromName(key));
  try {
    const r = await stub.fetch(`https://limiter/?limit=${limit}&period=${periodS}`);
    const j = await r.json();
    return j.success ? null : json({ error: "rate limited", retryAfter: j.retryAfter }, 429, { "retry-after": String(j.retryAfter) });
  } catch {
    return json({ error: "rate limiter unavailable" }, 503); // fail closed, like the gateway
  }
}

// Mutating API calls must come from our own pages: SameSite=Lax cookie plus an
// Origin check (and a JSON content type, which a plain HTML form can't send).
function sameOrigin(request, url) {
  const o = request.headers.get("origin");
  return o === url.origin && (request.headers.get("content-type") ?? "").startsWith("application/json");
}

async function cachedJson(request, ctx, ttl, produce) {
  const cache = caches.default;
  const key = new Request(new URL(request.url).origin + new URL(request.url).pathname, { method: "GET" });
  let r = await cache.match(key);
  if (r) return r;
  r = json(await produce(), 200, { "cache-control": `public, max-age=${ttl}` });
  ctx.waitUntil(cache.put(key, r.clone()));
  return r;
}

export async function handle(request, env, ctx) {
  const url = new URL(request.url);
  const path = url.pathname;
  const ip = request.headers.get("cf-connecting-ip") ?? "unknown";

  // ---- telemetry uplink ----
  if (path === "/ingest") {
    if (request.headers.get("upgrade") !== "websocket") return text("expected websocket", 426);
    if (!(await auth.verifyIngest(env.INGEST_SECRET, request.headers.get("authorization")))) return text("forbidden", 403);
    return hub(env).fetch("https://hub/ingest", request);
  }

  // ---- public read-only view ----
  if (path === "/ws") {
    if (request.headers.get("upgrade") !== "websocket") return text("expected websocket", 426);
    const rl = await limited(env, "ws:" + ip, 30);
    if (rl) return rl;
    return hub(env).fetch("https://hub/ws", request);
  }
  if (path === "/api/snapshot") {
    return cachedJson(request, ctx, 2, async () => (await hub(env).fetch("https://hub/snapshot")).json());
  }
  if (path === "/api/leaderboard") {
    const lb = env.LEADERBOARD.get(env.LEADERBOARD.idFromName("global"));
    return cachedJson(request, ctx, 30, async () => (await lb.fetch("https://lb/list")).json());
  }
  if (path === "/api/config") {
    return json({
      turnstileSiteKey: env.TURNSTILE_SITE_KEY ?? null,
      providers: Object.keys(auth.PROVIDERS).filter((p) => env[auth.PROVIDERS[p].idVar]),
    });
  }

  // ---- OAuth ----
  const am = /^\/auth\/(github|google)\/(start|callback)$/.exec(path);
  if (am) {
    const [, provider, step] = am;
    const p = auth.PROVIDERS[provider];
    if (!env[p.idVar] || !env[p.secretVar] || !env.SESSION_SECRET) return text(`${provider} sign-in isn't configured yet`, 503);
    if (step === "start") {
      if (request.method !== "POST") return text("method not allowed", 405);
      const rl = await limited(env, "auth:" + ip, 10);
      if (rl) return rl;
      const form = await request.formData().catch(() => null);
      const ok = await auth.verifyTurnstile(env, form?.get("cf-turnstile-response"), ip);
      if (!ok) return text("bot check failed, please try again", 403);
      const state = auth.randomState();
      return new Response(null, {
        status: 303,
        headers: {
          location: auth.authorizeUrl(provider, env, url.origin, state),
          "set-cookie": auth.setCookie("lle_oauth", state, 600, "/auth"),
        },
      });
    }
    const state = url.searchParams.get("state");
    const expected = auth.cookie(request.headers.get("cookie"), "lle_oauth");
    if (!state || !expected || state !== expected) return text("sign-in expired, please try again", 400);
    try {
      const user = await auth.fetchProfile(provider, env, url.origin, url.searchParams.get("code") ?? "");
      const sess = await auth.makeSession(env.SESSION_SECRET, user);
      const h = new Headers({ location: "/trade" });
      h.append("set-cookie", auth.setCookie(auth.SESSION_COOKIE, sess, 7 * 24 * 3600));
      h.append("set-cookie", auth.setCookie("lle_oauth", "", 0, "/auth"));
      return new Response(null, { status: 303, headers: h });
    } catch {
      return text("sign-in failed", 502);
    }
  }
  // Local development only: DEV_LOGIN lives in the gitignored .dev.vars, and the
  // route also refuses any host other than localhost.
  if (path === "/auth/dev" && request.method === "POST") {
    const local = ["localhost", "127.0.0.1"].includes(url.hostname);
    if (env.DEV_LOGIN !== "1" || !local || !env.SESSION_SECRET) return text("not found", 404);
    const sess = await auth.makeSession(env.SESSION_SECRET, { id: "dev:local", name: "local-dev", provider: "dev" });
    return new Response(null, { status: 303, headers: { location: "/trade", "set-cookie": auth.setCookie(auth.SESSION_COOKIE, sess, 3600) } });
  }
  if (path === "/auth/logout" && request.method === "POST") {
    if (!sameOrigin(request, url)) return text("forbidden", 403);
    return json({ ok: true }, 200, { "set-cookie": auth.setCookie(auth.SESSION_COOKIE, "", 0) });
  }

  // ---- signed-in paper accounts ----
  const acct = /^\/api\/(account|order|cancel|reset|privacy|delete)$/.exec(path);
  if (acct) {
    const user = await auth.readSession(env.SESSION_SECRET, request.headers.get("cookie"));
    if (!user) return json({ error: "sign in to trade (paper only)" }, 401);
    const op = acct[1];
    if (op !== "account") {
      if (request.method !== "POST") return text("method not allowed", 405);
      if (!sameOrigin(request, url)) return json({ error: "forbidden" }, 403);
    }
    const rl = await limited(env, "user:" + user.id, op === "account" ? 60 : 20);
    if (rl) return rl;
    let body = null;
    if (op !== "account") body = await request.text();
    if (body && body.length > 2048) return json({ error: "request too large" }, 413);
    if (op === "reset" || op === "delete") {
      const b = JSON.parse(body || "{}");
      if (!(await auth.verifyTurnstile(env, b.turnstile, ip))) return json({ error: "bot check failed" }, 403);
    }
    const stub = env.ACCOUNT.get(env.ACCOUNT.idFromName(user.id));
    const r = await stub.fetch(`https://account/${op === "account" ? "" : op}`, {
      method: op === "account" ? "GET" : "POST",
      headers: { "x-user": JSON.stringify(user) },
      body: op === "account" ? undefined : body,
    });
    const out = new Response(r.body, r);
    if (op === "delete") out.headers.append("set-cookie", auth.setCookie(auth.SESSION_COOKIE, "", 0));
    return out;
  }

  // ---- admin (Cloudflare Access) ----
  if (path === "/admin" || path.startsWith("/admin/")) {
    // DEV_ADMIN (gitignored .dev.vars only) skips Access for localhost requests.
    const devAdmin = env.DEV_ADMIN === "1" && ["localhost", "127.0.0.1"].includes(url.hostname);
    const v = devAdmin ? { ok: true } : await auth.verifyAccess(request, env);
    if (!v.ok) return text(v.msg, v.status);
    if (path === "/admin/api/command" && request.method === "POST") {
      const cmd = await request.json().catch(() => null);
      const ops = ["kill", "unkill", "start", "stop", "limits", "replay", "status"];
      if (!cmd || !ops.includes(cmd.op)) return json({ error: "unknown op" }, 400);
      const r = await hub(env).fetch("https://hub/admin", { method: "POST", body: JSON.stringify(cmd) });
      return new Response(r.body, { status: r.status, headers: JSON_HEADERS });
    }
    if (path === "/admin" || path === "/admin/") {
      // The asset server maps /admin to admin.html (html_handling); asking for
      // /admin.html itself would redirect back here.
      return env.ASSETS.fetch(new Request(new URL("/admin", url), request));
    }
    return text("not found", 404);
  }

  if (path === "/admin.html") return text("not found", 404); // only reachable through /admin
  return env.ASSETS.fetch(request);
}

export default {
  fetch(request, env, ctx) {
    return handle(request, env, ctx);
  },
};

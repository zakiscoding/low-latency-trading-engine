// Sessions, OAuth (GitHub, Google), Turnstile, Cloudflare Access, ingest tokens.
// No passwords are ever handled or stored. A session holds a stable user id
// (provider + provider user id, hashed) and a display handle; OAuth access tokens
// are used once to read the profile and then dropped.
import { createRemoteJWKSet, jwtVerify } from "jose";

const enc = new TextEncoder();

function b64url(bytes) {
  let s = "";
  for (const b of bytes) s += String.fromCharCode(b);
  return btoa(s).replace(/\+/g, "-").replace(/\//g, "_").replace(/=+$/, "");
}
function b64urlDecode(s) {
  const bin = atob(s.replace(/-/g, "+").replace(/_/g, "/"));
  return Uint8Array.from(bin, (c) => c.charCodeAt(0));
}

async function hmacKey(secret) {
  return crypto.subtle.importKey("raw", enc.encode(secret), { name: "HMAC", hash: "SHA-256" }, false, ["sign", "verify"]);
}
export async function hmacHex(secret, msg) {
  const sig = await crypto.subtle.sign("HMAC", await hmacKey(secret), enc.encode(msg));
  return [...new Uint8Array(sig)].map((b) => b.toString(16).padStart(2, "0")).join("");
}
function safeEqual(a, b) {
  if (a.length !== b.length) return false;
  let d = 0;
  for (let i = 0; i < a.length; i++) d |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return d === 0;
}
export async function sha256Hex(s) {
  const h = await crypto.subtle.digest("SHA-256", enc.encode(s));
  return [...new Uint8Array(h)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

// ---- sessions ----
export const SESSION_COOKIE = "lle_s";
const SESSION_TTL_S = 7 * 24 * 3600;

export async function makeSession(secret, user, nowS = Math.floor(Date.now() / 1000)) {
  const body = b64url(enc.encode(JSON.stringify({ u: user.id, n: user.name, p: user.provider, exp: nowS + SESSION_TTL_S })));
  return `${body}.${await hmacHex(secret, "session:" + body)}`;
}
export async function readSession(secret, cookieHeader, nowS = Math.floor(Date.now() / 1000)) {
  if (!secret) return null;
  const raw = cookie(cookieHeader, SESSION_COOKIE);
  if (!raw) return null;
  const [body, sig] = raw.split(".");
  if (!body || !sig) return null;
  if (!safeEqual(sig, await hmacHex(secret, "session:" + body))) return null;
  try {
    const s = JSON.parse(new TextDecoder().decode(b64urlDecode(body)));
    if (typeof s.exp !== "number" || s.exp < nowS) return null;
    return { id: s.u, name: s.n, provider: s.p };
  } catch {
    return null;
  }
}
export function cookie(header, name) {
  for (const part of (header ?? "").split(";")) {
    const [k, ...v] = part.trim().split("=");
    if (k === name) return v.join("=");
  }
  return null;
}
export function setCookie(name, value, maxAge, path = "/") {
  return `${name}=${value}; Path=${path}; Max-Age=${maxAge}; HttpOnly; Secure; SameSite=Lax`;
}

// ---- ingest (telemetry uplink) ----
// Token: "<unix_ts>.<hex hmac(secret, 'ingest:'+ts)>", valid for +-120 s.
export async function verifyIngest(secret, authHeader, nowS = Math.floor(Date.now() / 1000)) {
  if (!secret || secret.length < 32) return false;
  const m = /^Bearer (\d{9,12})\.([0-9a-f]{64})$/.exec(authHeader ?? "");
  if (!m) return false;
  const ts = Number(m[1]);
  if (Math.abs(nowS - ts) > 120) return false;
  return safeEqual(m[2], await hmacHex(secret, "ingest:" + m[1]));
}

// ---- Cloudflare Access (admin) ----
let jwks, jwksTeam;
export async function verifyAccess(request, env, keys) {
  if (!env.ACCESS_TEAM_DOMAIN || !env.ACCESS_AUD) return { ok: false, status: 503, msg: "Cloudflare Access isn't configured for admin yet." };
  const token = request.headers.get("cf-access-jwt-assertion");
  if (!token) return { ok: false, status: 403, msg: "A Cloudflare Access login is required." };
  if (!keys) {
    if (jwksTeam !== env.ACCESS_TEAM_DOMAIN) {
      jwks = createRemoteJWKSet(new URL(`${env.ACCESS_TEAM_DOMAIN}/cdn-cgi/access/certs`));
      jwksTeam = env.ACCESS_TEAM_DOMAIN;
    }
    keys = jwks;
  }
  try {
    const { payload } = await jwtVerify(token, keys, { issuer: env.ACCESS_TEAM_DOMAIN, audience: env.ACCESS_AUD });
    return { ok: true, email: payload.email };
  } catch {
    return { ok: false, status: 403, msg: "The Cloudflare Access token isn't valid." };
  }
}

// ---- Turnstile ----
export async function verifyTurnstile(env, token, ip, fetchFn = fetch) {
  if (!env.TURNSTILE_SECRET) return env.DEV_NO_TURNSTILE === "1";
  if (!token || token.length > 2048) return false;
  const body = new FormData();
  body.append("secret", env.TURNSTILE_SECRET);
  body.append("response", token);
  if (ip) body.append("remoteip", ip);
  const r = await fetchFn("https://challenges.cloudflare.com/turnstile/v0/siteverify", { method: "POST", body });
  const j = await r.json().catch(() => ({}));
  return j.success === true;
}

// ---- OAuth ----
export const PROVIDERS = {
  github: {
    authorize: "https://github.com/login/oauth/authorize",
    token: "https://github.com/login/oauth/access_token",
    scope: "", // public profile only
    idVar: "GITHUB_CLIENT_ID",
    secretVar: "GITHUB_CLIENT_SECRET",
  },
  google: {
    authorize: "https://accounts.google.com/o/oauth2/v2/auth",
    token: "https://oauth2.googleapis.com/token",
    scope: "openid profile",
    idVar: "GOOGLE_CLIENT_ID",
    secretVar: "GOOGLE_CLIENT_SECRET",
  },
};

export function authorizeUrl(provider, env, origin, state) {
  const p = PROVIDERS[provider];
  const u = new URL(p.authorize);
  u.searchParams.set("client_id", env[p.idVar]);
  u.searchParams.set("redirect_uri", `${origin}/auth/${provider}/callback`);
  u.searchParams.set("state", state);
  if (p.scope) u.searchParams.set("scope", p.scope);
  if (provider === "google") u.searchParams.set("response_type", "code");
  return u.toString();
}

// Exchanges the code and returns {id, name, provider}. The access token is used
// once and never stored.
export async function fetchProfile(provider, env, origin, code, fetchFn = fetch) {
  const p = PROVIDERS[provider];
  const form = new URLSearchParams({
    client_id: env[p.idVar],
    client_secret: env[p.secretVar],
    code,
    redirect_uri: `${origin}/auth/${provider}/callback`,
  });
  if (provider === "google") form.set("grant_type", "authorization_code");
  const tr = await fetchFn(p.token, { method: "POST", headers: { accept: "application/json" }, body: form });
  const tok = await tr.json().catch(() => ({}));
  if (provider === "github") {
    if (!tok.access_token) throw new Error("github token exchange failed");
    const ur = await fetchFn("https://api.github.com/user", {
      headers: { authorization: `Bearer ${tok.access_token}`, "user-agent": "paper-trading-demo", accept: "application/vnd.github+json" },
    });
    const u = await ur.json();
    if (!u.id || !u.login) throw new Error("github profile missing");
    return { id: "gh:" + (await sha256Hex("github:" + u.id)).slice(0, 24), name: String(u.login).slice(0, 39), provider };
  }
  // Google: the id_token comes straight from Google's token endpoint over TLS, so
  // its claims can be read without re-verifying the signature (OIDC Core 3.1.3.7).
  if (!tok.id_token) throw new Error("google token exchange failed");
  const claims = JSON.parse(new TextDecoder().decode(b64urlDecode(tok.id_token.split(".")[1])));
  if (claims.aud !== env[p.idVar] || !claims.sub) throw new Error("google id_token audience mismatch");
  const id = "go:" + (await sha256Hex("google:" + claims.sub)).slice(0, 24);
  const given = String(claims.given_name ?? "trader").replace(/[^\p{L}\p{N}_-]/gu, "").slice(0, 20) || "trader";
  return { id, name: `${given}-${id.slice(3, 7)}`, provider };
}

export function randomState() {
  const b = new Uint8Array(24);
  crypto.getRandomValues(b);
  return b64url(b);
}

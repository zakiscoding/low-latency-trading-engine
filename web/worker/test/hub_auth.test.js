import assert from "node:assert/strict";
import { test } from "node:test";
import * as auth from "../src/auth.js";
import { applyUpstream, bookFor, emptyHub } from "../src/hubstate.js";

test("hub applies snapshot then deltas", () => {
  const h = emptyHub();
  assert.equal(applyUpstream(h, { t: "d", seq: 1 }), false); // delta before snapshot ignored
  applyUpstream(h, {
    t: "snap",
    seq: 5,
    bids: [[100, 1], [99, 2]],
    asks: [[101, 3]],
    trades: [{ ts: 1, p: 100, q: 1, s: "b" }],
    latency: { tick_to_trade: { n: 1, buckets: [[1000, 1]] } },
    stats: {},
  });
  applyUpstream(h, {
    t: "d",
    seq: 6,
    bids: [[100, 0], [98, 4]],
    asks: [[101, 1], [102, 5]],
    trades: [{ ts: 2, p: 101, q: 2, s: "a" }],
    lat_add: [[1000, 2], [2000, 1]],
    lat: { n: 4, p50: 1000, p99: 2000, p999: 2000, max: 1900 },
    stats: { orders: 3 },
  });
  assert.deepEqual(h.snap.bids, [[99, 2], [98, 4]]);
  assert.deepEqual(h.snap.asks, [[101, 1], [102, 5]]);
  assert.equal(h.snap.trades.length, 2);
  assert.deepEqual(h.snap.latency.tick_to_trade.buckets, [[1000, 3], [2000, 1]]);
  assert.equal(h.snap.latency.tick_to_trade.n, 4);
  assert.equal(h.snap.seq, 6);
  const b = bookFor(h, 0);
  assert.equal(b.trades.length, 2);
  assert.equal(bookFor(h, b.tradeSeq).trades.length, 0);
  // A later snapshot re-sends the first trade: not logged twice.
  applyUpstream(h, { t: "snap", seq: 7, bids: [], asks: [], trades: [{ ts: 1, p: 100, q: 1, s: "b" }] });
  assert.equal(bookFor(h, 0).trades.length, 2);
});

test("sessions: sign, verify, tamper, expiry", async () => {
  const secret = "s".repeat(40);
  const tok = await auth.makeSession(secret, { id: "gh:abc", name: "zaki", provider: "github" }, 1000);
  const hdr = `foo=1; ${auth.SESSION_COOKIE}=${tok}`;
  assert.deepEqual(await auth.readSession(secret, hdr, 1001), { id: "gh:abc", name: "zaki", provider: "github" });
  assert.equal(await auth.readSession("x".repeat(40), hdr, 1001), null);
  assert.equal(await auth.readSession(secret, hdr.replace(/.$/, (c) => (c === "0" ? "1" : "0")), 1001), null);
  assert.equal(await auth.readSession(secret, hdr, 1000 + 8 * 24 * 3600), null);
  assert.equal(await auth.readSession(undefined, hdr, 1001), null);
});

test("ingest token matches the Go signer and expires", async () => {
  const secret = "secret-secret-secret-secret-1234";
  const ts = "1794631681";
  const good = `Bearer ${ts}.${await auth.hmacHex(secret, "ingest:" + ts)}`;
  assert.equal(await auth.verifyIngest(secret, good, 1794631681 + 60), true);
  assert.equal(await auth.verifyIngest(secret, good, 1794631681 + 500), false);
  assert.equal(await auth.verifyIngest("short", good, 1794631681), false);
  assert.equal(await auth.verifyIngest(secret, "Bearer nope", 1794631681), false);
});

test("access fails closed when unconfigured or missing token", async () => {
  const req = new Request("https://x/admin");
  assert.equal((await auth.verifyAccess(req, {})).status, 503);
  assert.equal((await auth.verifyAccess(req, { ACCESS_TEAM_DOMAIN: "https://t.cloudflareaccess.com", ACCESS_AUD: "a" })).status, 403);
});

test("turnstile: fails closed without a secret unless dev flag set", async () => {
  assert.equal(await auth.verifyTurnstile({}, "tok"), false);
  assert.equal(await auth.verifyTurnstile({ DEV_NO_TURNSTILE: "1" }, "tok"), true);
  const fake = async () => new Response(JSON.stringify({ success: true }));
  assert.equal(await auth.verifyTurnstile({ TURNSTILE_SECRET: "x" }, "tok", "1.2.3.4", fake), true);
  assert.equal(await auth.verifyTurnstile({ TURNSTILE_SECRET: "x" }, "", "1.2.3.4", fake), false);
});

test("github profile: token used once, id hashed, no token kept", async () => {
  const calls = [];
  const fake = async (url, init) => {
    calls.push(url);
    if (url.includes("access_token")) return new Response(JSON.stringify({ access_token: "gho_x" }));
    return new Response(JSON.stringify({ id: 42, login: "octocat", email: "hidden@example.com" }));
  };
  const u = await auth.fetchProfile("github", { GITHUB_CLIENT_ID: "id", GITHUB_CLIENT_SECRET: "s" }, "https://x", "code", fake);
  assert.equal(u.name, "octocat");
  assert.match(u.id, /^gh:[0-9a-f]{24}$/);
  assert.equal(JSON.stringify(u).includes("gho_x"), false);
  assert.equal(JSON.stringify(u).includes("hidden@"), false);
  assert.equal(calls.length, 2);
});

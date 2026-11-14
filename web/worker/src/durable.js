// Durable Objects: MarketHub (fan-out + edge cache), Account (per-user paper
// simulator), Leaderboard (weekly paper P&L), ClientRateLimiter (sliding window).
import { DurableObject } from "cloudflare:workers";
import { applyUpstream, bookFor, candlesView, emptyHub } from "./hubstate.js";
import { metricsView } from "./metrics.js";
import * as sim from "./sim.js";

const MAX_VIEWERS = 1000;

// One instance ("global"). The telemetry service on the home machine dials in to
// /ingest (outbound from home) and is the only upstream; every viewer is served
// from here, so viewers never reach the home machine.
export class MarketHub extends DurableObject {
  constructor(ctx, env) {
    super(ctx, env);
    this.hub = emptyHub();
    this.pending = new Map(); // admin command id -> resolve
    this.paused = false;
    ctx.blockConcurrencyWhile(async () => {
      const bars = await ctx.storage.get("candles");
      if (bars) Object.assign(this.hub, { candles: bars.candles ?? [], candleWm: bars.wm ?? 0, candleWmKeys: bars.wmKeys ?? [] });
      const met = await ctx.storage.get("metrics");
      if (met) Object.assign(this.hub.metrics, { rows: met.rows ?? [], markouts: met.markouts ?? [], seen: met.seen ?? [] });
      const saved = await ctx.storage.get("snap");
      if (saved) applyUpstream(this.hub, saved);
      this.hub.candlesDirty = false;
      this.hub.metricsDirty = false;
    });
  }

  upstream() {
    return this.ctx.getWebSockets("up")[0] ?? null;
  }
  viewers() {
    return this.ctx.getWebSockets("v");
  }

  async fetch(request) {
    const url = new URL(request.url);
    if (url.pathname === "/ingest") {
      const pair = new WebSocketPair();
      for (const old of this.ctx.getWebSockets("up")) old.close(1012, "replaced");
      this.ctx.acceptWebSocket(pair[1], ["up"]);
      this.paused = false;
      if (this.viewers().length === 0) this.sendUp({ t: "pause" });
      return new Response(null, { status: 101, webSocket: pair[0] });
    }
    if (url.pathname === "/ws") {
      if (this.viewers().length >= MAX_VIEWERS) return new Response("too many viewers", { status: 503 });
      const pair = new WebSocketPair();
      this.ctx.acceptWebSocket(pair[1], ["v"]);
      if (this.hub.snap) pair[1].send(JSON.stringify(this.publicSnap()));
      else pair[1].send(JSON.stringify({ t: "offline" }));
      // Always (re)send resume: `paused` lives in memory and is lost if the DO was evicted.
      this.sendUp({ t: "resume" });
      this.paused = false;
      return new Response(null, { status: 101, webSocket: pair[0] });
    }
    if (url.pathname === "/snapshot") {
      return Response.json(this.hub.snap ? this.publicSnap() : { t: "offline" });
    }
    if (url.pathname === "/metrics") {
      return Response.json(metricsView(this.hub.metrics, this.hub.snap, Date.now()));
    }
    if (url.pathname === "/candles") {
      return Response.json(candlesView(this.hub));
    }
    if (url.pathname === "/book") {
      return Response.json(bookFor(this.hub, Number(url.searchParams.get("since") ?? 0)));
    }
    if (url.pathname === "/admin" && request.method === "POST") {
      const cmd = await request.json();
      return Response.json(await this.admin(cmd));
    }
    return new Response("not found", { status: 404 });
  }

  // Only what viewers may see. Engine internals (ring loss etc.) stay in stats, which
  // contain nothing sensitive (no hostnames, IPs or keys ever enter the stream).
  publicSnap() {
    return { ...this.hub.snap, upstream: !!this.upstream() };
  }

  sendUp(obj) {
    const up = this.upstream();
    if (up) {
      try {
        up.send(JSON.stringify(obj));
      } catch {}
    }
  }

  async admin(cmd) {
    const up = this.upstream();
    if (!up) return { ok: false, error: "engine offline (no telemetry uplink)" };
    const id = crypto.randomUUID();
    const p = new Promise((resolve) => {
      this.pending.set(id, resolve);
      setTimeout(() => {
        if (this.pending.delete(id)) resolve({ ok: false, error: "timed out waiting for the engine" });
      }, 5000);
    });
    up.send(JSON.stringify({ t: "admin", id, op: cmd.op, limits: cmd.limits }));
    return p;
  }

  async webSocketMessage(ws, message) {
    const tags = this.ctx.getTags(ws);
    if (!tags.includes("up")) return; // viewers are read-only
    let msg;
    try {
      msg = JSON.parse(message);
    } catch {
      return;
    }
    if (msg.t === "ack") {
      const r = this.pending.get(msg.id);
      if (r) {
        this.pending.delete(msg.id);
        r(msg);
      }
      return;
    }
    if (!applyUpstream(this.hub, msg)) return;
    if (msg.t === "snap") await this.ctx.storage.put("snap", this.hub.snap);
    if (this.hub.candlesDirty) {
      // A new 1-minute bar started: persist the closed ones (about one write a minute).
      this.hub.candlesDirty = false;
      await this.ctx.storage.put("candles", { candles: this.hub.candles, wm: this.hub.candleWm, wmKeys: this.hub.candleWmKeys });
    }
    if (this.hub.metricsDirty) {
      this.hub.metricsDirty = false;
      const m = this.hub.metrics;
      await this.ctx.storage.put("metrics", { rows: m.rows, markouts: m.markouts, seen: m.seen });
    }
    const out = msg.t === "snap" ? JSON.stringify(this.publicSnap()) : message;
    for (const v of this.viewers()) {
      try {
        v.send(out);
      } catch {}
    }
  }

  async webSocketClose(ws) {
    const tags = this.ctx.getTags(ws);
    if (tags.includes("v") && this.viewers().filter((v) => v !== ws).length === 0) {
      // Nobody watching: stop deltas (the uplink still sends a snapshot every 30 s,
      // which keeps the edge copy fresh for the REST snapshot).
      this.sendUp({ t: "pause" });
      this.paused = true;
    }
    if (tags.includes("up")) {
      for (const v of this.viewers()) {
        try {
          v.send(JSON.stringify({ t: "upstream", up: false }));
        } catch {}
      }
    }
  }
  async webSocketError(ws) {
    await this.webSocketClose(ws);
  }
}

// One per signed-in user, keyed by user id. Holds only the handle and paper balances.
export class Account extends DurableObject {
  async load() {
    const s = await this.ctx.storage.get("acct");
    return s ? sim.deserialize(s) : null;
  }
  async save(a) {
    await this.ctx.storage.put("acct", sim.serialize(a));
  }

  async hubBook(since) {
    const stub = this.env.HUB.get(this.env.HUB.idFromName("global"));
    const r = await stub.fetch(`https://hub/book?since=${since}`);
    return r.json();
  }

  async sync(a) {
    const book = await this.hubBook(a.tradeSeq);
    // If the hub restarted, its trade sequence starts over.
    if (book.tradeSeq < a.tradeSeq) a.tradeSeq = 0;
    sim.applyMarket(a, book, book.trades, Date.now());
    return book;
  }

  async report(a, book, user) {
    if (!user) return;
    const lb = this.env.LEADERBOARD.get(this.env.LEADERBOARD.idFromName("global"));
    const prof = (await this.ctx.storage.get("profile")) ?? {};
    const week = sim.weekKey(Date.now());
    let base = await this.ctx.storage.get("weekBase");
    const eq = sim.equityMicros(a, book);
    if (!base || base.week !== week) {
      base = { week, equity: eq.toString() };
      await this.ctx.storage.put("weekBase", base);
    }
    await lb.fetch("https://lb/update", {
      method: "POST",
      body: JSON.stringify({
        id: user.id,
        name: user.name,
        hidden: !!prof.hidden,
        week,
        pnl: (eq - BigInt(base.equity)).toString(),
      }),
    });
  }

  async fetch(request) {
    const url = new URL(request.url);
    const user = JSON.parse(request.headers.get("x-user") ?? "null");
    if (!user) return new Response("no user", { status: 400 });
    let a = await this.load();
    if (!a) {
      a = sim.newAccount(Date.now());
      await this.ctx.storage.put("profile", { id: user.id, name: user.name, provider: user.provider, hidden: false });
    }
    const book = await this.sync(a);
    let result = null;
    if (url.pathname === "/order" && request.method === "POST") {
      if (!book.fresh) result = { error: "market data is stale, orders are paused" };
      else result = sim.placeOrder(a, book, await request.json(), Date.now());
    } else if (url.pathname === "/cancel" && request.method === "POST") {
      const { id } = await request.json();
      result = { cancelled: sim.cancelOrder(a, Number(id)) };
    } else if (url.pathname === "/reset" && request.method === "POST") {
      a = sim.newAccount(Date.now());
      a.tradeSeq = book.tradeSeq;
      await this.ctx.storage.delete("weekBase");
      result = { reset: true };
    } else if (url.pathname === "/privacy" && request.method === "POST") {
      const { hidden } = await request.json();
      const prof = (await this.ctx.storage.get("profile")) ?? {};
      prof.hidden = !!hidden;
      await this.ctx.storage.put("profile", prof);
      result = { hidden: prof.hidden };
    } else if (url.pathname === "/delete" && request.method === "POST") {
      await this.ctx.storage.deleteAll();
      const lb = this.env.LEADERBOARD.get(this.env.LEADERBOARD.idFromName("global"));
      await lb.fetch("https://lb/remove", { method: "POST", body: JSON.stringify({ id: user.id }) });
      return Response.json({ deleted: true });
    }
    await this.save(a);
    await this.report(a, book, user);
    // Keep evaluating resting orders while any are open.
    if (a.open.length) await this.ctx.storage.setAlarm(Date.now() + 10_000);
    const prof = (await this.ctx.storage.get("profile")) ?? {};
    const body = { result: serializeResult(result), account: sim.view(a, book), hidden: !!prof.hidden, user: { name: user.name } };
    return Response.json(body, { status: result?.error ? 400 : 200 });
  }

  async alarm() {
    const a = await this.load();
    if (!a || !a.open.length) return;
    const book = await this.sync(a);
    await this.save(a);
    const prof = (await this.ctx.storage.get("profile")) ?? {};
    if (prof.id) await this.report(a, book, { id: prof.id, name: prof.name, provider: prof.provider });
    if (a.open.length) await this.ctx.storage.setAlarm(Date.now() + 10_000);
  }
}

function serializeResult(r) {
  if (!r) return r;
  return JSON.parse(JSON.stringify(r, (_, v) => (typeof v === "bigint" ? v.toString() : v)));
}

export class Leaderboard extends DurableObject {
  async fetch(request) {
    const url = new URL(request.url);
    if (url.pathname === "/update") {
      const e = await request.json();
      await this.ctx.storage.put("u:" + e.id, { name: e.name, hidden: e.hidden, week: e.week, pnl: e.pnl, ts: Date.now() });
      return new Response("ok");
    }
    if (url.pathname === "/remove") {
      const { id } = await request.json();
      await this.ctx.storage.delete("u:" + id);
      return new Response("ok");
    }
    const week = sim.weekKey(Date.now());
    const all = await this.ctx.storage.list({ prefix: "u:" });
    const rows = [...all.values()]
      .filter((r) => r.week === week && !r.hidden)
      .sort((x, y) => (BigInt(y.pnl) > BigInt(x.pnl) ? 1 : BigInt(y.pnl) < BigInt(x.pnl) ? -1 : 0))
      .slice(0, 25)
      .map((r, i) => ({ rank: i + 1, name: r.name, pnl_micros: r.pnl }));
    return Response.json({ week, rows, traders: [...all.values()].filter((r) => r.week === week).length });
  }
}

// Same sliding-window limiter as gpu-systems-lab/gateway (it counts requests per key).
export class ClientRateLimiter extends DurableObject {
  async fetch(request) {
    const u = new URL(request.url);
    const limit = Number(u.searchParams.get("limit"));
    const periodMs = Number(u.searchParams.get("period") ?? 60) * 1000;
    const now = Date.now();
    const recent = ((await this.ctx.storage.get("requests")) ?? []).filter((t) => t > now - periodMs);
    if (recent.length >= limit) {
      const retryAfter = Math.max(1, Math.ceil((recent[recent.length - limit] + periodMs - now) / 1000));
      return Response.json({ success: false, retryAfter });
    }
    recent.push(now);
    await this.ctx.storage.put("requests", recent);
    return Response.json({ success: true });
  }
}

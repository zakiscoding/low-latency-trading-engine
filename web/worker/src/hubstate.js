import { emptyMetrics, trackMetrics } from "./metrics.js";

// Keeps the edge copy of the market view current by applying telemetry deltas to
// the last snapshot. Pure functions (unit tested); the MarketHub DO holds the state.

const DEPTH = 20;
const KEEP_TRADES = 60;
const KEEP_FILLS = 40;
const KEEP_TRADE_LOG = 1000;
export const CANDLE_MS = 60_000;
export const KEEP_CANDLES = 2 * 24 * 60; // two days of 1-minute bars

function applyLevels(levels, changes, desc) {
  const m = new Map(levels.map(([p, q]) => [p, q]));
  for (const [p, q] of changes ?? []) {
    if (q === 0) m.delete(p);
    else m.set(p, q);
  }
  return [...m.entries()].sort((a, b) => (desc ? b[0] - a[0] : a[0] - b[0])).slice(0, DEPTH);
}

function mergeBuckets(buckets, add) {
  const m = new Map((buckets ?? []).map(([ns, c]) => [ns, c]));
  for (const [ns, c] of add ?? []) m.set(ns, (m.get(ns) ?? 0) + c);
  return [...m.entries()].sort((a, b) => a[0] - b[0]);
}

export function emptyHub() {
  return { snap: null, seq: 0, tradeSeq: 0, tradeLog: [], candles: [], candleWm: 0, candleWmKeys: [], metrics: emptyMetrics() };
}

// 1-minute OHLCV bars built from the trades the engine streams: [startMs, o, h, l, c, v]
// with prices in the feed's integer ticks and volume in its qty units. Trades can
// arrive twice (snapshots repeat the last 60), so a timestamp watermark plus the
// keys seen at exactly that timestamp keeps each trade counted once. Returns true
// when a new bar was started (the caller persists then, at most once a minute).
export function foldCandle(hub, t) {
  if (!(t.ts > 0) || !(t.p > 0) || !(t.q > 0)) return false;
  const key = `${t.p}:${t.q}:${t.s}`;
  if (t.ts < hub.candleWm) return false;
  if (t.ts === hub.candleWm) {
    if (hub.candleWmKeys.includes(key)) return false;
    hub.candleWmKeys.push(key);
  } else {
    hub.candleWm = t.ts;
    hub.candleWmKeys = [key];
  }
  const start = Math.floor(t.ts / 1e6 / CANDLE_MS) * CANDLE_MS;
  const c = hub.candles;
  const last = c[c.length - 1];
  if (last && last[0] === start) {
    if (t.p > last[2]) last[2] = t.p;
    if (t.p < last[3]) last[3] = t.p;
    last[4] = t.p;
    last[5] += t.q;
    return false;
  }
  if (last && start < last[0]) return false; // late trade for a closed bar
  c.push([start, t.p, t.p, t.p, t.p, t.q]);
  if (c.length > KEEP_CANDLES) c.splice(0, c.length - KEEP_CANDLES);
  return true;
}

// Returns true if the message changed the cached view.
export function applyUpstream(hub, msg, now = Date.now()) {
  if (msg.t === "snap") {
    hub.snap = msg;
    hub.seq = msg.seq;
    for (const t of msg.trades ?? []) {
      logTrade(hub, t, true);
      if (foldCandle(hub, t)) hub.candlesDirty = true;
    }
    hub.snap.received = now;
    if (trackMetrics(hub.metrics, hub.snap, msg, now)) hub.metricsDirty = true;
    return true;
  }
  if (msg.t !== "d" || !hub.snap) return false;
  const s = hub.snap;
  s.bids = applyLevels(s.bids ?? [], msg.bids, true);
  s.asks = applyLevels(s.asks ?? [], msg.asks, false);
  if (msg.trades?.length) {
    s.trades = [...(s.trades ?? []), ...msg.trades].slice(-KEEP_TRADES);
    for (const t of msg.trades) {
      logTrade(hub, t, false);
      if (foldCandle(hub, t)) hub.candlesDirty = true;
    }
  }
  if (msg.fills?.length) s.fills = [...(s.fills ?? []), ...msg.fills].slice(-KEEP_FILLS);
  if (msg.lat_add?.length) {
    const t2t = s.latency?.tick_to_trade ?? { n: 0, buckets: [] };
    t2t.buckets = mergeBuckets(t2t.buckets, msg.lat_add);
    if (msg.lat) Object.assign(t2t, { n: msg.lat.n, p50: msg.lat.p50, p99: msg.lat.p99, p999: msg.lat.p999, max: msg.lat.max });
    s.latency = { ...(s.latency ?? {}), tick_to_trade: t2t };
  }
  if (msg.stats) s.stats = msg.stats;
  s.seq = msg.seq;
  s.ts = msg.ts;
  s.received = now;
  hub.seq = msg.seq;
  if (trackMetrics(hub.metrics, s, msg, now)) hub.metricsDirty = true;
  return true;
}

// Trades from snapshots overlap ones already logged; dedupe by (ts, p, q).
function logTrade(hub, t, fromSnap) {
  if (fromSnap && hub.tradeLog.some((x) => x.ts === t.ts && x.p === t.p && x.q === t.q)) return;
  hub.tradeSeq++;
  hub.tradeLog.push({ seq: hub.tradeSeq, ts: t.ts, p: t.p, q: t.q, s: t.s });
  if (hub.tradeLog.length > KEEP_TRADE_LOG) hub.tradeLog.splice(0, hub.tradeLog.length - KEEP_TRADE_LOG);
}

export function candlesView(hub) {
  return {
    tf_ms: CANDLE_MS,
    price_dec: hub.snap?.meta?.price_dec ?? 2,
    qty_dec: hub.snap?.meta?.qty_dec ?? 8,
    candles: hub.candles,
    wm: hub.candleWm, // last trade timestamp folded in (ns) and the trade keys seen at it
    wm_keys: hub.candleWmKeys,
  };
}

// What a paper account needs: top of book and trades after `since`.
export function bookFor(hub, since) {
  const s = hub.snap;
  return {
    bids: s?.bids ?? [],
    asks: s?.asks ?? [],
    fresh: !!s && Date.now() - (s.received ?? 0) < 15_000,
    trades: hub.tradeLog.filter((t) => t.seq > since),
    tradeSeq: hub.tradeSeq,
  };
}

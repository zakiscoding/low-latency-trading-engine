// Keeps the edge copy of the market view current by applying telemetry deltas to
// the last snapshot. Pure functions (unit tested); the MarketHub DO holds the state.

const DEPTH = 20;
const KEEP_TRADES = 60;
const KEEP_FILLS = 40;
const KEEP_TRADE_LOG = 1000;

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
  return { snap: null, seq: 0, tradeSeq: 0, tradeLog: [] };
}

// Returns true if the message changed the cached view.
export function applyUpstream(hub, msg, now = Date.now()) {
  if (msg.t === "snap") {
    hub.snap = msg;
    hub.seq = msg.seq;
    for (const t of msg.trades ?? []) logTrade(hub, t, true);
    hub.snap.received = now;
    return true;
  }
  if (msg.t !== "d" || !hub.snap) return false;
  const s = hub.snap;
  s.bids = applyLevels(s.bids ?? [], msg.bids, true);
  s.asks = applyLevels(s.asks ?? [], msg.asks, false);
  if (msg.trades?.length) {
    s.trades = [...(s.trades ?? []), ...msg.trades].slice(-KEEP_TRADES);
    for (const t of msg.trades) logTrade(hub, t, false);
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
  return true;
}

// Trades from snapshots overlap ones already logged; dedupe by (ts, p, q).
function logTrade(hub, t, fromSnap) {
  if (fromSnap && hub.tradeLog.some((x) => x.ts === t.ts && x.p === t.p && x.q === t.q)) return;
  hub.tradeSeq++;
  hub.tradeLog.push({ seq: hub.tradeSeq, ts: t.ts, p: t.p, q: t.q, s: t.s });
  if (hub.tradeLog.length > KEEP_TRADE_LOG) hub.tradeLog.splice(0, hub.tradeLog.length - KEEP_TRADE_LOG);
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

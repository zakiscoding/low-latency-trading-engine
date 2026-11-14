// Per-minute engine metrics for the quant/monitoring view, built in the MarketHub
// from the telemetry stream it already receives (no new data sources):
//   - tick-to-trade p50/p99/p99.9/max per minute: diff of the cumulative histogram
//   - messages / orders / fills / risk rejects / ring loss per minute: diffs of the
//     engine's cumulative counters (a decrease means the engine restarted)
//   - demo-strategy P&L, fees and position at the end of each minute
//   - uplink health: updates received and the longest gap between them
//   - markouts of the engine's own paper fills: mid 1 s, 5 s and 60 s after each
//     fill vs the fill price, signed so positive = the fill was good for us, plus
//     the effective half-spread paid against the mid at fill time
// Pure functions; unit tested.

export const MINUTE_MS = 60_000;
export const KEEP_ROWS = 24 * 60;
const KEEP_MIDS = 400; // ~6.5 minutes of 1 s samples
const KEEP_MARKOUTS = 300;
const HORIZONS = [1, 5, 60]; // seconds

export function emptyMetrics() {
  return { rows: [], cur: null, mids: [], pending: [], markouts: [], seen: [] };
}

function bucketsOf(snap) {
  return snap?.latency?.tick_to_trade?.buckets ?? [];
}

function newCur(t, stats, buckets) {
  return { t, base: stats ?? null, last: stats ?? null, baseBuckets: buckets.map((b) => b.slice()), updates: 0, lastAt: 0, maxGap: 0 };
}

// Percentiles from [[upper_ns, count]] (conservative: bucket upper edges).
export function percentiles(buckets) {
  const sorted = buckets.filter((b) => b[1] > 0).sort((a, b) => a[0] - b[0]);
  const n = sorted.reduce((s, b) => s + b[1], 0);
  if (!n) return { n: 0 };
  const at = (p) => {
    const want = Math.ceil((p / 100) * n);
    let c = 0;
    for (const [ns, k] of sorted) if ((c += k) >= want) return ns;
    return sorted[sorted.length - 1][0];
  };
  return { n, p50: at(50), p99: at(99), p999: at(99.9), max: sorted[sorted.length - 1][0] };
}

function diffBuckets(now, base) {
  const m = new Map(base.map(([ns, c]) => [ns, c]));
  let restarted = false;
  const out = [];
  for (const [ns, c] of now) {
    const d = c - (m.get(ns) ?? 0);
    if (d < 0) restarted = true;
    if (d > 0) out.push([ns, d]);
  }
  return restarted ? now.map((b) => b.slice()) : out;
}

const diff = (last, base, k) => {
  if (!last || last[k] === undefined) return 0;
  if (!base || base[k] === undefined) return 0;
  return last[k] >= base[k] ? last[k] - base[k] : last[k]; // counter reset = engine restart
};

export function strategyPnl(st, meta) {
  if (!st || st.cash_micros === undefined) return null;
  const pd = meta?.price_dec ?? 2,
    qd = meta?.qty_dec ?? 8;
  return Math.round(st.cash_micros + ((st.position ?? 0) * (st.mark_mid ?? 0) * 1e6) / 10 ** (pd + qd));
}

function finish(m, meta, buckets) {
  const c = m.cur;
  const lat = percentiles(diffBuckets(buckets, c.baseBuckets));
  m.rows.push({
    t: c.t,
    lat,
    msgs: diff(c.last, c.base, "messages"),
    orders: diff(c.last, c.base, "orders"),
    fills: diff(c.last, c.base, "fills"),
    rejects: diff(c.last, c.base, "rejects"),
    lost: diff(c.last, c.base, "ring_lost"),
    pos: c.last?.position ?? null,
    pnl: strategyPnl(c.last, meta),
    fees: c.last?.fees_micros ?? null,
    up: c.last?.engine_up !== false,
    kill: !!c.last?.kill,
    updates: c.updates,
    gap: c.maxGap,
  });
  if (m.rows.length > KEEP_ROWS) m.rows.splice(0, m.rows.length - KEEP_ROWS);
}

// Called after the hub applied an upstream message. Returns true when a minute
// row was closed (the caller persists then).
export function trackMetrics(m, snap, msg, now) {
  if (!snap) return false;
  const minute = Math.floor(now / MINUTE_MS) * MINUTE_MS;
  const buckets = bucketsOf(snap);
  let closed = false;
  if (!m.cur) m.cur = newCur(minute, snap.stats, buckets);
  if (m.cur.t !== minute) {
    finish(m, snap.meta, buckets);
    m.cur = newCur(minute, m.cur.last, buckets);
    closed = true;
  }
  const c = m.cur;
  if (c.lastAt) c.maxGap = Math.max(c.maxGap, now - c.lastAt);
  c.lastAt = now;
  c.updates++;
  if (msg.stats) c.last = msg.stats;
  if (!c.base && msg.stats) c.base = msg.stats;

  // Mid samples, at most one a second.
  const bb = snap.bids?.[0]?.[0],
    ba = snap.asks?.[0]?.[0];
  if (bb && ba) {
    const mid = (bb + ba) / 2;
    const sec = Math.floor(now / 1000) * 1000;
    const last = m.mids[m.mids.length - 1];
    if (!last || last[0] !== sec) m.mids.push([sec, mid]);
    else last[1] = mid;
    if (m.mids.length > KEEP_MIDS) m.mids.splice(0, m.mids.length - KEEP_MIDS);
  }

  // New engine fills (snapshots repeat recent ones).
  for (const f of msg.fills ?? []) {
    const key = `${f.ts}:${f.p}:${f.q}:${f.s}`;
    if (m.seen.includes(key)) continue;
    m.seen.push(key);
    if (m.seen.length > 200) m.seen.splice(0, m.seen.length - 200);
    const tMs = Math.round(f.ts / 1e6);
    if (now - tMs > 120_000) continue; // too old to mark out (e.g. a snapshot after a restart)
    m.pending.push({ tMs, side: f.s === "b" ? "buy" : "sell", p: f.p, q: f.q, fee: f.fee, liq: f.liq });
  }
  // Mark out fills whose longest horizon has passed.
  const done = [];
  m.pending = m.pending.filter((x) => {
    if (now < x.tMs + HORIZONS[HORIZONS.length - 1] * 1000 + 1500) return true;
    done.push(x);
    return false;
  });
  for (const x of done) {
    const sgn = x.side === "buy" ? 1 : -1;
    const mo = {};
    for (const h of HORIZONS) {
      const mid = midAt(m.mids, x.tMs + h * 1000, 1500);
      mo[h] = mid === null ? null : sgn * (mid - x.p);
    }
    const mid0 = midAt(m.mids, x.tMs, 1500);
    m.markouts.push({ t: x.tMs, side: x.side, p: x.p, q: x.q, fee: x.fee, liq: x.liq, spread: mid0 === null ? null : sgn * (mid0 - x.p), mo });
  }
  if (m.markouts.length > KEEP_MARKOUTS) m.markouts.splice(0, m.markouts.length - KEEP_MARKOUTS);
  return closed;
}

// Mid sample nearest to t (samples are 1 s apart), if within `tol` ms; else null.
export function midAt(mids, t, tol) {
  let best = null;
  for (const [s, mid] of mids) if (best === null || Math.abs(s - t) < Math.abs(best[0] - t)) best = [s, mid];
  return best && Math.abs(best[0] - t) <= tol ? best[1] : null;
}

export function metricsView(m, snap, now) {
  const avg = (k) => {
    const v = m.markouts.map((x) => x.mo[k]).filter((x) => x !== null);
    return v.length ? { n: v.length, avg: v.reduce((a, b) => a + b, 0) / v.length } : { n: 0 };
  };
  const sp = m.markouts.map((x) => x.spread).filter((x) => x !== null);
  return {
    now,
    meta: snap?.meta ?? null,
    rows: m.rows,
    markouts: m.markouts.slice(-100),
    markout_avg: { 1: avg(1), 5: avg(5), 60: avg(60), spread: sp.length ? { n: sp.length, avg: sp.reduce((a, b) => a + b, 0) / sp.length } : { n: 0 } },
    pending_fills: m.pending.length,
  };
}

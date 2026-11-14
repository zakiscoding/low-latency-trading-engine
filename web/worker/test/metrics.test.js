import assert from "node:assert/strict";
import { test } from "node:test";
import { emptyMetrics, metricsView, midAt, percentiles, trackMetrics } from "../src/metrics.js";

const meta = { price_dec: 2, qty_dec: 8 };
const snapAt = (buckets, stats, bid = 10000, ask = 10002) => ({ meta, bids: [[bid, 1]], asks: [[ask, 1]], latency: { tick_to_trade: { buckets } }, stats });

test("percentiles from bucket counts", () => {
  assert.deepEqual(percentiles([]), { n: 0 });
  const p = percentiles([[1000, 98], [5000, 1], [20000, 1]]);
  assert.equal(p.n, 100);
  assert.equal(p.p50, 1000);
  assert.equal(p.p99, 5000);
  assert.equal(p.p999, 20000);
  assert.equal(p.max, 20000);
});

test("minute rows: counter and histogram diffs, restart, gaps", () => {
  const m = emptyMetrics();
  const t0 = 1_800_000_000_000 - (1_800_000_000_000 % 60_000);
  const st = (msgs, orders) => ({ messages: msgs, orders, fills: 0, rejects: 0, ring_lost: 0, position: 0, cash_micros: 0, mark_mid: 10001 });
  trackMetrics(m, snapAt([[1000, 5]], st(100, 1)), { t: "snap", stats: st(100, 1) }, t0 + 1000);
  trackMetrics(m, snapAt([[1000, 7], [9000, 1]], st(400, 4)), { t: "d", stats: st(400, 4) }, t0 + 11_000);
  assert.equal(m.rows.length, 0);
  // Next minute closes the first row.
  assert.equal(trackMetrics(m, snapAt([[1000, 7], [9000, 1]], st(450, 4)), { t: "d", stats: st(450, 4) }, t0 + 61_000), true);
  const r = m.rows[0];
  assert.equal(r.msgs, 300);
  assert.equal(r.orders, 3);
  assert.equal(r.lat.n, 3); // 2 more at 1 µs, 1 at 9 µs during the minute
  assert.equal(r.lat.max, 9000);
  assert.equal(r.gap, 10_000);
  assert.equal(r.updates, 2);
  // Engine restart: counters and histogram start over.
  trackMetrics(m, snapAt([[1000, 1]], st(20, 0)), { t: "snap", stats: st(20, 0) }, t0 + 90_000);
  trackMetrics(m, snapAt([[1000, 1]], st(25, 0)), { t: "d", stats: st(25, 0) }, t0 + 121_000);
  assert.equal(m.rows[1].msgs, 20);
  assert.equal(m.rows[1].lat.n, 1);
});

test("markouts: signed mid move after each fill, deduped", () => {
  const m = emptyMetrics();
  const t = 1_800_000_000_000;
  // Mid 100.01 at the fill, 100.03 one second later and after.
  trackMetrics(m, snapAt([], {}), { t: "d" }, t);
  const fill = { ts: t * 1e6, s: "b", p: 10002, q: 1000, fee: 3, liq: "T" };
  trackMetrics(m, snapAt([], {}), { t: "d", fills: [fill] }, t + 100);
  for (let s = 1; s <= 62; s++) trackMetrics(m, snapAt([], {}, 10002, 10004), { t: "d", fills: s === 3 ? [fill] : [] }, t + s * 1000);
  assert.equal(m.markouts.length, 1);
  const x = m.markouts[0];
  assert.equal(x.side, "buy");
  assert.equal(x.spread, -1); // paid 1 tick over mid
  assert.equal(x.mo[1], 1); // mid moved up 1 tick from the fill price
  assert.equal(x.mo[60], 1);
  assert.equal(midAt([[1000, 5]], 9000, 1000), null);
  const v = metricsView(m, snapAt([], {}), t + 70_000);
  assert.equal(v.markout_avg[5].n, 1);
});

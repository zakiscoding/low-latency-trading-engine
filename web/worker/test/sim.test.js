import assert from "node:assert/strict";
import { test } from "node:test";
import * as sim from "../src/sim.js";

const BOOK = {
  bids: [
    [6700000, 50_000_000], // $67,000.00 x 0.5
    [6699900, 100_000_000],
  ],
  asks: [
    [6700100, 30_000_000], // $67,001.00 x 0.3
    [6700200, 200_000_000],
  ],
};
const NOW = Date.UTC(2026, 10, 14, 12);

test("market buy walks the asks and charges taker fee", () => {
  const a = sim.newAccount(NOW);
  const r = sim.placeOrder(a, BOOK, { side: "buy", type: "market", qty: "0.33" }, NOW);
  assert.equal(r.filled, 33_000_000n);
  assert.equal(a.fills.length, 2);
  assert.equal(a.fills[0].price, 6700100n);
  assert.equal(a.fills[0].qty, 30_000_000n);
  assert.equal(a.fills[1].qty, 3_000_000n);
  const n1 = sim.notionalMicros(6700100n, 30_000_000n); // $20,100.30
  assert.equal(n1, 20_100_300_000n);
  const n2 = sim.notionalMicros(6700200n, 3_000_000n);
  const fees = sim.feeMicros(n1, false) + sim.feeMicros(n2, false);
  assert.equal(a.cash, sim.START_CASH_MICROS - n1 - n2 - fees);
  assert.equal(a.btc, 33_000_000n);
});

test("validation: qty, price band, cash, no shorting, daily cap", () => {
  const a = sim.newAccount(NOW);
  assert.match(sim.placeOrder(a, BOOK, { side: "buy", type: "market", qty: "5" }, NOW).error, /qty/);
  assert.match(sim.placeOrder(a, BOOK, { side: "buy", type: "market", qty: "1e-3" }, NOW).error, /qty/);
  assert.match(sim.placeOrder(a, BOOK, { side: "sell", type: "market", qty: "0.01" }, NOW).error, /shorting/);
  assert.match(sim.placeOrder(a, BOOK, { side: "buy", type: "limit", qty: "0.01", price: "10.00" }, NOW).error, /far/);
  assert.match(sim.placeOrder(a, BOOK, { side: "buy", type: "limit", qty: "0.01", price: "abc" }, NOW).error, /price/);
  assert.match(sim.placeOrder(a, { bids: [], asks: [] }, { side: "buy", type: "market", qty: "0.01" }, NOW).error, /unavailable/);
  a.ordersToday = sim.LIMITS.maxOrdersPerDay;
  assert.match(sim.placeOrder(a, BOOK, { side: "buy", type: "market", qty: "0.01" }, NOW).error, /daily/);
  // New day resets the counter.
  assert.equal(sim.placeOrder(a, BOOK, { side: "buy", type: "market", qty: "0.01" }, NOW + 86_400_000).error, undefined);
});

test("notional cap", () => {
  const a = sim.newAccount(NOW);
  // 0.4 BTC at ~$67k = $26.8k > $25k
  assert.match(sim.placeOrder(a, BOOK, { side: "buy", type: "limit", qty: "0.4", price: "67000.00" }, NOW).error, /25,000/);
});

test("resting bid waits for the queue, then fills as maker", () => {
  const a = sim.newAccount(NOW);
  const r = sim.placeOrder(a, BOOK, { side: "buy", type: "limit", qty: "0.1", price: "67000.00" }, NOW);
  assert.equal(r.filled, 0n);
  assert.equal(a.open.length, 1);
  assert.equal(a.open[0].ahead, 50_000_000n);
  // 0.3 traded at our price against bids: burns queue only.
  sim.applyMarket(a, BOOK, [{ seq: 1, p: 6700000, q: 30_000_000, s: "b" }], NOW);
  assert.equal(a.fills.length, 0);
  assert.equal(a.open[0].ahead, 20_000_000n);
  // Asks side trades don't touch our bid.
  sim.applyMarket(a, BOOK, [{ seq: 2, p: 6700000, q: 99_000_000, s: "a" }], NOW);
  assert.equal(a.fills.length, 0);
  // 0.25 more: 0.2 ahead, 0.05 fills us.
  sim.applyMarket(a, BOOK, [{ seq: 3, p: 6700000, q: 25_000_000, s: "b" }], NOW);
  assert.equal(a.fills.length, 1);
  assert.equal(a.fills[0].qty, 5_000_000n);
  assert.equal(a.fills[0].liq, "M");
  // A trade below our bid fills the rest.
  sim.applyMarket(a, BOOK, [{ seq: 4, p: 6699000, q: 1, s: "b" }], NOW);
  assert.equal(a.open.length, 0);
  assert.equal(a.btc, 10_000_000n);
  // Already-seen trades are ignored.
  sim.applyMarket(a, BOOK, [{ seq: 4, p: 6600000, q: 1, s: "b" }], NOW);
  assert.equal(a.fills.length, 2);
});

test("touch crossing fills a resting order; cancels shrink the queue", () => {
  const a = sim.newAccount(NOW);
  sim.placeOrder(a, BOOK, { side: "buy", type: "limit", qty: "0.1", price: "67000.00" }, NOW);
  const smaller = { ...BOOK, bids: [[6700000, 10_000_000], ...BOOK.bids.slice(1)] };
  sim.applyMarket(a, smaller, [], NOW);
  assert.equal(a.open[0].ahead, 10_000_000n);
  const crossed = { bids: BOOK.bids, asks: [[6700000, 5_000_000]] };
  sim.applyMarket(a, crossed, [], NOW);
  assert.equal(a.open.length, 0);
  assert.equal(a.btc, 10_000_000n);
});

test("reserved cash prevents double spending with resting orders", () => {
  const a = sim.newAccount(NOW);
  for (let i = 0; i < 3; i++) {
    const r = sim.placeOrder(a, BOOK, { side: "buy", type: "limit", qty: "0.35", price: "66000.00" }, NOW);
    if (i < 4) assert.equal(r.error, undefined);
  }
  // 3 x $23.1k reserved ~ $69.3k; a 4th and 5th would exceed $100k.
  sim.placeOrder(a, BOOK, { side: "buy", type: "limit", qty: "0.35", price: "66000.00" }, NOW);
  const r = sim.placeOrder(a, BOOK, { side: "buy", type: "limit", qty: "0.35", price: "66000.00" }, NOW);
  assert.match(r.error, /cash/);
});

test("orders expire, cancel works, serialize round-trips bigints", () => {
  const a = sim.newAccount(NOW);
  const r = sim.placeOrder(a, BOOK, { side: "buy", type: "limit", qty: "0.01", price: "66000.00" }, NOW);
  assert.equal(sim.cancelOrder(a, r.id), true);
  assert.equal(sim.cancelOrder(a, r.id), false);
  sim.placeOrder(a, BOOK, { side: "buy", type: "limit", qty: "0.01", price: "66000.00" }, NOW);
  const b = sim.deserialize(sim.serialize(a));
  assert.equal(b.cash, a.cash);
  assert.equal(typeof b.open[0].qty, "bigint");
  sim.applyMarket(b, BOOK, [], NOW + sim.LIMITS.orderTtlMs + 1);
  assert.equal(b.open.length, 0);
});

test("week key is Monday UTC", () => {
  assert.equal(sim.weekKey(Date.UTC(2026, 10, 14)), "2026-11-09"); // Saturday -> Monday 9th
  assert.equal(sim.weekKey(Date.UTC(2026, 10, 9, 0, 0, 1)), "2026-11-09");
  assert.equal(sim.weekKey(Date.UTC(2026, 10, 8, 23, 59)), "2026-11-02");
});

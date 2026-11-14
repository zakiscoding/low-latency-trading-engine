// Per-user paper trading simulator (pure functions, unit tested).
//
// Fill model, the same rules as the C++ paper exchange:
//  - Market orders and the marketable part of limit orders walk the displayed
//    opposite side of the live book (taker fee).
//  - The rest of a limit order rests with queue_ahead = displayed size at its price.
//    Real trades at that price on that side burn queue_ahead first, then fill us;
//    a trade through our price, or the opposite touch crossing it, fills us fully
//    (maker fee).
//  - Cash account: no leverage, no shorting. Buys need cash, sells need inventory.
// All amounts are integers: price in cents, qty in 1e-8 BTC, money in micro-dollars.

export const QTY_SCALE = 100_000_000n; // 1 BTC
export const START_CASH_MICROS = 100_000n * 1_000_000n; // $100,000 paper

export const LIMITS = {
  minQty: 10_000n, // 0.0001 BTC
  maxQty: 50_000_000n, // 0.5 BTC per order
  maxNotionalMicros: 25_000n * 1_000_000n, // $25,000 per order
  maxOpenOrders: 20,
  maxOrdersPerDay: 200,
  maxPriceBandCents: 500_000n, // limit price within $5,000 of the touch
  orderTtlMs: 24 * 3600 * 1000,
};

// Bitstamp entry-tier fees in parts per million of notional.
export const FEES = { makerPpm: 3000n, takerPpm: 4000n };

// notional micro-dollars = cents * qty / 1e8 * 1e4
export function notionalMicros(priceCents, qty) {
  return (BigInt(priceCents) * BigInt(qty) * 10_000n) / QTY_SCALE;
}
export function feeMicros(notional, maker) {
  return (notional * (maker ? FEES.makerPpm : FEES.takerPpm)) / 1_000_000n;
}

export function parseQty(s) {
  if (typeof s !== "string" || !/^\d{1,3}(\.\d{1,8})?$/.test(s)) return null;
  const [w, f = ""] = s.split(".");
  return BigInt(w) * QTY_SCALE + BigInt((f + "00000000").slice(0, 8));
}
export function parsePrice(s) {
  if (typeof s !== "string" || !/^\d{1,7}(\.\d{1,2})?$/.test(s)) return null;
  const [w, f = ""] = s.split(".");
  return BigInt(w) * 100n + BigInt((f + "00").slice(0, 2));
}

export function newAccount(now) {
  return {
    cash: START_CASH_MICROS,
    btc: 0n,
    fees: 0n,
    open: [], // {id, side, price, qty, ahead, ts}
    fills: [], // last 50
    nextId: 1,
    day: dayKey(now),
    ordersToday: 0,
    tradeSeq: 0, // last hub trade sequence applied
    createdAt: now,
  };
}

export function dayKey(ms) {
  return new Date(ms).toISOString().slice(0, 10);
}

// Monday 00:00 UTC of the week containing ms, as YYYY-MM-DD.
export function weekKey(ms) {
  const d = new Date(ms);
  const dow = (d.getUTCDay() + 6) % 7;
  return new Date(Date.UTC(d.getUTCFullYear(), d.getUTCMonth(), d.getUTCDate() - dow)).toISOString().slice(0, 10);
}

function recordFill(acct, f) {
  acct.fills.push(f);
  if (acct.fills.length > 50) acct.fills.splice(0, acct.fills.length - 50);
}

function applyFill(acct, side, price, qty, maker, ts, orderId) {
  const n = notionalMicros(price, qty);
  const fee = feeMicros(n, maker);
  if (side === "buy") {
    acct.cash -= n + fee;
    acct.btc += qty;
  } else {
    acct.cash += n - fee;
    acct.btc -= qty;
  }
  acct.fees += fee;
  recordFill(acct, { orderId, side, price, qty, fee, liq: maker ? "M" : "T", ts });
}

// Qty already reserved by resting orders (so they can't be double-spent).
function reserved(acct) {
  let cash = 0n,
    btc = 0n;
  for (const o of acct.open) {
    if (o.side === "buy") cash += notionalMicros(o.price, o.qty) + feeMicros(notionalMicros(o.price, o.qty), false);
    else btc += o.qty;
  }
  return { cash, btc };
}

// book: {bids: [[cents, qty]...] best first, asks: [...]} with numbers or bigints.
export function placeOrder(acct, book, req, now) {
  if (acct.day !== dayKey(now)) {
    acct.day = dayKey(now);
    acct.ordersToday = 0;
  }
  const side = req.side;
  if (side !== "buy" && side !== "sell") return { error: "side must be buy or sell" };
  const type = req.type === "limit" ? "limit" : req.type === "market" ? "market" : null;
  if (!type) return { error: "type must be market or limit" };
  const qty = parseQty(req.qty);
  if (qty === null || qty < LIMITS.minQty || qty > LIMITS.maxQty) return { error: "qty must be 0.0001-0.5 BTC" };
  if (acct.ordersToday >= LIMITS.maxOrdersPerDay) return { error: "daily order limit reached" };
  if (type === "limit" && acct.open.length >= LIMITS.maxOpenOrders) return { error: "too many open orders" };
  const bids = (book?.bids ?? []).map(([p, q]) => [BigInt(p), BigInt(q)]);
  const asks = (book?.asks ?? []).map(([p, q]) => [BigInt(p), BigInt(q)]);
  if (!bids.length || !asks.length) return { error: "market data unavailable, try again shortly" };
  const touch = side === "buy" ? asks[0][0] : bids[0][0];
  let limit = null;
  if (type === "limit") {
    limit = parsePrice(req.price);
    if (limit === null || limit <= 0n) return { error: "price must be dollars with up to 2 decimals" };
    const d = limit > touch ? limit - touch : touch - limit;
    if (d > LIMITS.maxPriceBandCents) return { error: "limit price too far from the market" };
  }
  const worst = limit ?? (side === "buy" ? touch + LIMITS.maxPriceBandCents : touch - LIMITS.maxPriceBandCents);
  const estNotional = notionalMicros(worst > 0n ? worst : touch, qty);
  if (estNotional > LIMITS.maxNotionalMicros) return { error: "order larger than the $25,000 paper limit" };
  const res = reserved(acct);
  if (side === "buy" && estNotional + feeMicros(estNotional, false) > acct.cash - res.cash)
    return { error: "not enough paper cash" };
  if (side === "sell" && qty > acct.btc - res.btc) return { error: "not enough paper BTC (no shorting)" };

  acct.ordersToday++;
  const id = acct.nextId++;
  let left = qty;
  const levels = side === "buy" ? asks : bids;
  for (const [p, q] of levels) {
    if (left === 0n) break;
    if (limit !== null && (side === "buy" ? p > limit : p < limit)) break;
    if (limit === null && (side === "buy" ? p > worst : p < worst)) break;
    const take = q < left ? q : left;
    if (take <= 0n) continue;
    applyFill(acct, side, p, take, false, now, id);
    left -= take;
  }
  let rested = null;
  if (left > 0n && type === "limit") {
    const own = side === "buy" ? bids : asks;
    const at = own.find(([p]) => p === limit);
    rested = { id, side, price: limit, qty: left, ahead: at ? at[1] : 0n, ts: now };
    acct.open.push(rested);
  }
  return { id, filled: qty - left, rested: rested ? rested.qty : 0n, cancelled: type === "market" ? left : 0n };
}

export function cancelOrder(acct, id) {
  const i = acct.open.findIndex((o) => o.id === id);
  if (i < 0) return false;
  acct.open.splice(i, 1);
  return true;
}

// trades: [{seq, p, q, s}] where s is the side of the resting order that was hit
// ("b" = a bid was hit by a seller, "a" = an ask was lifted by a buyer).
export function applyMarket(acct, book, trades, now) {
  let filled = 0;
  for (const t of trades) {
    if (t.seq <= acct.tradeSeq) continue;
    acct.tradeSeq = t.seq;
    const p = BigInt(t.p);
    let q = BigInt(t.q);
    for (const o of [...acct.open]) {
      const wantSide = o.side === "buy" ? "b" : "a";
      if (t.s !== wantSide) continue;
      const through = o.side === "buy" ? p < o.price : p > o.price;
      if (through) {
        applyFill(acct, o.side, o.price, o.qty, true, now, o.id);
        o.qty = 0n;
        filled++;
        continue;
      }
      if (p !== o.price || q <= 0n) continue;
      const burn = o.ahead < q ? o.ahead : q;
      o.ahead -= burn;
      q -= burn;
      if (q <= 0n) continue;
      const take = q < o.qty ? q : o.qty;
      applyFill(acct, o.side, o.price, take, true, now, o.id);
      o.qty -= take;
      q -= take;
      filled++;
    }
    acct.open = acct.open.filter((o) => o.qty > 0n);
  }
  // Opposite touch crossed a resting order: it would have traded.
  if (book?.bids?.length && book?.asks?.length) {
    const bestBid = BigInt(book.bids[0][0]),
      bestAsk = BigInt(book.asks[0][0]);
    for (const o of [...acct.open]) {
      if ((o.side === "buy" && bestAsk <= o.price) || (o.side === "sell" && bestBid >= o.price)) {
        applyFill(acct, o.side, o.price, o.qty, true, now, o.id);
        o.qty = 0n;
        filled++;
      }
      // Rule: cancels ahead of us shrink the queue.
      const own = o.side === "buy" ? book.bids : book.asks;
      const lvl = own.find(([p]) => BigInt(p) === o.price);
      const shown = lvl ? BigInt(lvl[1]) : 0n;
      if (o.ahead > shown) o.ahead = shown;
    }
    acct.open = acct.open.filter((o) => o.qty > 0n);
  }
  // Expire old orders.
  acct.open = acct.open.filter((o) => now - o.ts < LIMITS.orderTtlMs);
  return filled;
}

export function equityMicros(acct, book) {
  if (!book?.bids?.length || !book?.asks?.length) return acct.cash;
  const mid = (BigInt(book.bids[0][0]) + BigInt(book.asks[0][0])) / 2n;
  return acct.cash + notionalMicros(mid, acct.btc);
}

// JSON-safe view (bigints -> strings).
export function view(acct, book) {
  const s = (v) => v.toString();
  return {
    cash_micros: s(acct.cash),
    btc: s(acct.btc),
    fees_micros: s(acct.fees),
    equity_micros: s(equityMicros(acct, book)),
    pnl_micros: s(equityMicros(acct, book) - START_CASH_MICROS),
    open: acct.open.map((o) => ({ id: o.id, side: o.side, price: s(o.price), qty: s(o.qty), ahead: s(o.ahead), ts: o.ts })),
    fills: acct.fills.map((f) => ({ ...f, price: s(f.price), qty: s(f.qty), fee: s(f.fee) })),
    orders_today: acct.ordersToday,
    limits: {
      max_qty_btc: "0.5",
      max_notional_usd: 25000,
      max_open_orders: LIMITS.maxOpenOrders,
      max_orders_per_day: LIMITS.maxOrdersPerDay,
    },
  };
}

// Storage round trip: bigints are stored as strings.
export function serialize(acct) {
  return JSON.stringify(acct, (_, v) => (typeof v === "bigint" ? { $b: v.toString() } : v));
}
export function deserialize(s) {
  return JSON.parse(s, (_, v) => (v && typeof v === "object" && typeof v.$b === "string" ? BigInt(v.$b) : v));
}

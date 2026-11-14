// Candles: 1-minute bars come from the hub (/api/candles), built from the trades
// the engine streams; live trades are folded in on the client the same way.
// Bar: [startMs, open, high, low, close, volume] in feed ticks / qty units.
export const MINUTE = 60_000;

export function emptyCandles() {
  return { bars: [], wm: 0, wmKeys: [] };
}

export function fromServer(j) {
  const bars = (j?.candles ?? []).map((b) => b.slice());
  return { bars, wm: j?.wm ?? 0, wmKeys: (j?.wm_keys ?? []).slice() };
}

// Same dedupe rule as the hub: timestamp watermark plus keys seen at it.
export function fold(cs, trades) {
  if (!trades?.length) return cs;
  const out = { bars: cs.bars.slice(), wm: cs.wm, wmKeys: cs.wmKeys.slice() };
  for (const t of trades) {
    if (!(t.ts > 0) || !(t.p > 0) || !(t.q > 0) || t.ts < out.wm) continue;
    const key = `${t.p}:${t.q}:${t.s}`;
    if (t.ts === out.wm) {
      if (out.wmKeys.includes(key)) continue;
      out.wmKeys.push(key);
    } else {
      out.wm = t.ts;
      out.wmKeys = [key];
    }
    const start = Math.floor(t.ts / 1e6 / MINUTE) * MINUTE;
    const last = out.bars[out.bars.length - 1];
    if (last && last[0] === start) {
      const b = last.slice();
      if (t.p > b[2]) b[2] = t.p;
      if (t.p < b[3]) b[3] = t.p;
      b[4] = t.p;
      b[5] += t.q;
      out.bars[out.bars.length - 1] = b;
    } else if (!last || start > last[0]) {
      out.bars.push([start, t.p, t.p, t.p, t.p, t.q]);
    }
  }
  if (out.bars.length > 2880) out.bars.splice(0, out.bars.length - 2880);
  return out;
}

// Groups 1-minute bars into `minutes`-minute bars aligned to the epoch.
export function aggregate(bars, minutes) {
  if (minutes <= 1) return bars;
  const w = minutes * MINUTE;
  const out = [];
  for (const b of bars) {
    const s = Math.floor(b[0] / w) * w;
    const last = out[out.length - 1];
    if (last && last[0] === s) {
      if (b[2] > last[2]) last[2] = b[2];
      if (b[3] < last[3]) last[3] = b[3];
      last[4] = b[4];
      last[5] += b[5];
    } else out.push([s, b[1], b[2], b[3], b[4], b[5]]);
  }
  return out;
}

// Change / high / low / volume over the last 24 h, or over whatever history the
// hub has if that is shorter (spanMs says how much).
export function rollingStats(bars, now = Date.now()) {
  if (!bars.length) return null;
  const from = now - 24 * 3600 * 1000;
  let i = bars.findIndex((b) => b[0] >= from);
  if (i < 0) i = bars.length - 1;
  const win = bars.slice(i);
  let hi = -Infinity,
    lo = Infinity,
    vol = 0;
  for (const b of win) {
    if (b[2] > hi) hi = b[2];
    if (b[3] < lo) lo = b[3];
    vol += b[5];
  }
  const open = win[0][1],
    last = win[win.length - 1][4];
  return { open, last, hi, lo, vol, change: last - open, pct: open ? ((last - open) / open) * 100 : 0, spanMs: now - win[0][0] };
}

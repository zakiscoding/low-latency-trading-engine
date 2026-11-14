// Engine monitoring: the view a quant desk keeps open next to a strategy. Dense
// grid of per-minute time series from /api/metrics (built in the hub from the
// telemetry stream) plus live state from the market stream.
import { useEffect, useMemo, useRef, useState } from "react";
import { LatencyHist } from "./charts.jsx";
import { fmt } from "./market.js";

const css = (n) => getComputedStyle(document.documentElement).getPropertyValue(n).trim();
const RANGES = [
  ["1h", 60],
  ["6h", 360],
  ["24h", 1440],
];

function useMetrics() {
  const [m, setM] = useState(null);
  useEffect(() => {
    let stop = false;
    const load = () =>
      fetch("/api/metrics")
        .then((r) => r.json())
        .then((j) => !stop && setM(j))
        .catch(() => {});
    load();
    const t = setInterval(() => !document.hidden && load(), 15000);
    return () => {
      stop = true;
      clearInterval(t);
    };
  }, []);
  return m;
}

// Small time-series chart: lines, steps or bars on a shared time axis, optional
// log scale, hover readout of every series at the nearest minute.
function Series({ series, from, to, height = 130, log = false, fmtV = (v) => v.toLocaleString(), zero = true }) {
  const ref = useRef(null);
  const [hx, setHx] = useState(null);
  useEffect(() => {
    const c = ref.current;
    if (!c) return;
    const draw = () => {
      const dpr = window.devicePixelRatio || 1;
      const W = c.clientWidth,
        H = height;
      c.width = W * dpr;
      c.height = H * dpr;
      const g = c.getContext("2d");
      g.setTransform(dpr, 0, 0, dpr, 0, 0);
      g.clearRect(0, 0, W, H);
      g.font = `10.5px ${css("--mono")}`;
      const L = 4,
        R = 56,
        T = 6,
        B = 16;
      const pw = W - L - R,
        ph = H - T - B;
      const pts = series.flatMap((s) => s.values.filter(([t, v]) => t >= from && v !== null && v !== undefined && (!log || v > 0)));
      g.fillStyle = css("--grid");
      g.fillRect(L, H - B, pw, 1);
      if (!pts.length) {
        g.fillStyle = css("--muted");
        g.fillText("no data in this range yet", L + 6, T + 14);
        return;
      }
      let lo = Math.min(...pts.map((p) => p[1])),
        hi = Math.max(...pts.map((p) => p[1]));
      if (zero && !log) lo = Math.min(0, lo);
      if (log) {
        lo = Math.log10(lo);
        hi = Math.log10(hi);
      }
      if (hi === lo) {
        const d = Math.abs(hi) * 0.05 || 1;
        hi += d;
        lo -= d;
      }
      const pad = (hi - lo) * 0.08;
      hi += pad;
      if (!(zero && !log && lo === 0)) lo -= pad;
      const X = (t) => L + ((t - from) / (to - from)) * pw;
      const Y = (v) => T + (1 - ((log ? Math.log10(v) : v) - lo) / (hi - lo)) * ph;
      // y grid: 3 lines
      g.textBaseline = "middle";
      for (let i = 0; i <= 2; i++) {
        const fv = lo + ((hi - lo) * i) / 2;
        const v = log ? 10 ** fv : fv;
        const y = Math.round(Y(v)) + 0.5;
        g.fillStyle = css("--grid");
        g.fillRect(L, y, pw, 1);
        g.fillStyle = css("--muted");
        g.fillText(fmtV(v), L + pw + 4, y);
      }
      // time labels
      g.textBaseline = "alphabetic";
      g.textAlign = "center";
      const span = to - from;
      const stepMs = span <= 3_600_000 ? 15 * 60_000 : span <= 6 * 3_600_000 ? 3_600_000 : 4 * 3_600_000;
      for (let t = Math.ceil(from / stepMs) * stepMs; t <= to; t += stepMs) {
        const d = new Date(t);
        g.fillStyle = css("--muted");
        g.fillText(`${String(d.getHours()).padStart(2, "0")}:${String(d.getMinutes()).padStart(2, "0")}`, X(t), H - 3);
      }
      g.textAlign = "left";
      const bw = Math.max(1, (pw / ((to - from) / 60_000)) * 0.8);
      for (const s of series) {
        const vals = s.values.filter(([t, v]) => t >= from && v !== null && v !== undefined && (!log || v > 0));
        g.strokeStyle = g.fillStyle = s.color;
        if (s.kind === "bar") {
          g.globalAlpha = 0.75;
          for (const [t, v] of vals) {
            const y0 = Y(log ? 10 ** lo : Math.max(lo, 0)),
              y1 = Y(v);
            g.fillRect(X(t) - bw / 2, Math.min(y0, y1), bw, Math.max(1, Math.abs(y0 - y1)));
          }
          g.globalAlpha = 1;
        } else {
          g.lineWidth = 1.4;
          g.beginPath();
          let prev = null;
          for (const [t, v] of vals) {
            const x = X(t),
              y = Y(v);
            // break the line across gaps of more than 3 minutes
            if (!prev || t - prev[0] > 180_000) g.moveTo(x, y);
            else if (s.kind === "step") {
              g.lineTo(x, Y(prev[1]));
              g.lineTo(x, y);
            } else g.lineTo(x, y);
            prev = [t, v];
          }
          g.stroke();
        }
      }
      if (hx !== null) {
        g.fillStyle = css("--cross");
        g.fillRect(Math.round(hx) + 0.5, T, 1, ph);
      }
    };
    draw();
    const ro = new ResizeObserver(draw);
    ro.observe(c);
    return () => ro.disconnect();
  });
  // hover readout
  let readout = null;
  if (hx !== null && ref.current) {
    const W = ref.current.clientWidth - 60;
    const t = from + ((hx - 4) / W) * (to - from);
    const near = (vals) => {
      let best = null;
      for (const p of vals) if (best === null || Math.abs(p[0] - t) < Math.abs(best[0] - t)) best = p;
      return best && Math.abs(best[0] - t) < 120_000 ? best : null;
    };
    const parts = series.map((s) => [s, near(s.values)]).filter(([, p]) => p);
    if (parts.length)
      readout = (
        <div className="readout">
          <span className="muted">{new Date(parts[0][1][0]).toLocaleTimeString("en-GB", { hour: "2-digit", minute: "2-digit" })}</span>
          {parts.map(([s, p]) => (
            <span key={s.name} style={{ color: s.color }}>
              {s.name} {p[1] === null ? "—" : fmtV(p[1])}
            </span>
          ))}
        </div>
      );
  }
  return (
    <div className="series" onPointerMove={(e) => setHx(e.clientX - e.currentTarget.getBoundingClientRect().left)} onPointerLeave={() => setHx(null)}>
      <canvas ref={ref} style={{ height }} />
      {readout}
    </div>
  );
}

function Tile({ title, sub, children, span = 1 }) {
  return (
    <section className={`tile span${span}`}>
      <div className="tile-h">
        <span>{title}</span>
        {sub && <span className="muted">{sub}</span>}
      </div>
      {children}
    </section>
  );
}

function Stat({ label, value, cls = "" }) {
  return (
    <div className="stat">
      <span>{label}</span>
      <b className={cls}>{value}</b>
    </div>
  );
}

export function EnginePage({ state }) {
  const m = useMetrics();
  const [picked, setRange] = useState(null);
  const meta = m?.meta ?? state?.meta;
  const pd = meta?.price_dec ?? 2,
    qd = meta?.qty_dec ?? 8;
  const rows = m?.rows ?? [];
  const to = m?.now ?? Date.now();
  // Until picked: the smallest range that covers the recorded history.
  const range = picked ?? (RANGES.find(([, mins]) => !rows.length || to - rows[0].t <= mins * 60_000) ?? RANGES[RANGES.length - 1])[1];
  const from = to - range * 60_000;
  const col = useMemo(
    () => ({
      a: css("--accent"),
      b: css("--bid"),
      r: css("--ask"),
      w: css("--warn"),
      v: "#8f7cf0",
      m: css("--muted"),
    }),
    [],
  );
  const S = (k, f = (r) => r[k]) => rows.map((r) => [r.t, f(r)]);
  const st = state?.stats ?? {};
  const t2t = state?.latency?.tick_to_trade;
  const age = state?.ts ? Math.max(0, Date.now() - state.ts / 1e6) : null;
  const totals = rows.filter((r) => r.t >= from).reduce((a, r) => ({ msgs: a.msgs + r.msgs, orders: a.orders + r.orders, fills: a.fills + r.fills, rejects: a.rejects + r.rejects, lost: a.lost + r.lost }), { msgs: 0, orders: 0, fills: 0, rejects: 0, lost: 0 });
  const usdTicks = (v) => (v === null || v === undefined ? "—" : `${v >= 0 ? "+" : "-"}$${(Math.abs(v) / 10 ** pd).toFixed(pd)}`);
  const mo = m?.markout_avg;
  const pnlNow = st.cash_micros !== undefined ? st.cash_micros + ((st.position ?? 0) * (st.mark_mid ?? 0) * 1e6) / 10 ** (pd + qd) : null;
  const engineUp = state?.upstream !== false && st.engine_up !== false && !state?.offline;

  return (
    <div className="engine">
      <div className="engine-bar">
        <div className="statrow">
          <Stat label="Engine" value={engineUp ? "UP" : "DOWN"} cls={engineUp ? "bid" : "ask"} />
          <Stat label="Strategy" value={st.kill ? "KILLED" : st.trading === false ? "STOPPED" : "RUNNING"} cls={st.kill ? "ask" : st.trading === false ? "warn" : "bid"} />
          <Stat label="Last update" value={age === null ? "—" : age < 1500 ? "live" : `${(age / 1000).toFixed(0)} s ago`} cls={age !== null && age > 35_000 ? "ask" : ""} />
          <Stat label="t2t p50 / p99" value={t2t?.n ? `${fmt.ns(t2t.p50)} / ${fmt.ns(t2t.p99)}` : "no samples"} />
          <Stat label="Position" value={st.position !== undefined ? `${fmt.qty(st.position, qd)} ${meta?.product?.split("-")[0] ?? ""}` : "—"} />
          <Stat label="Demo P&L (incl. fees)" value={pnlNow === null ? "—" : fmt.usd(pnlNow)} cls={pnlNow >= 0 ? "bid" : "ask"} />
          <Stat label={`Fill / order (${RANGES.find((r) => r[1] === range)[0]})`} value={totals.orders ? `${((totals.fills / totals.orders) * 100).toFixed(0)}%` : "—"} />
          <Stat label="Ring loss" value={(st.ring_lost ?? 0).toLocaleString()} cls={st.ring_lost ? "warn" : ""} />
        </div>
        <div className="intervals">
          {RANGES.map(([l, mins]) => (
            <button key={l} className={range === mins ? "on" : ""} onClick={() => setRange(mins)}>{l}</button>
          ))}
        </div>
      </div>
      {!m && <p className="empty">Loading metrics…</p>}
      {m && !rows.length && (
        <p className="empty">
          Metrics are recorded per minute by the edge hub from the live stream; the first rows appear a minute after this
          version is deployed.
        </p>
      )}
      <div className="tiles">
        <Tile title="Tick-to-trade per minute" sub="log scale · in-process, rdtsc" span={2}>
          <Series
            log
            from={from}
            to={to}
            fmtV={(v) => fmt.ns(v)}
            series={[
              { name: "p50", color: col.b, values: S("p50", (r) => r.lat?.p50 ?? null) },
              { name: "p99", color: col.w, values: S("p99", (r) => r.lat?.p99 ?? null) },
              { name: "p99.9", color: col.r, values: S("p999", (r) => r.lat?.p999 ?? null) },
            ]}
          />
        </Tile>
        <Tile title="Latency distribution" sub={t2t?.n ? `${t2t.n.toLocaleString()} orders since engine start` : "no samples yet"}>
          <LatencyHist buckets={t2t?.buckets} marks={t2t ?? {}} height={130} />
        </Tile>
        <Tile title="Feed messages / min" sub={`${totals.msgs.toLocaleString()} in range`}>
          <Series from={from} to={to} fmtV={(v) => Math.round(v).toLocaleString()} series={[{ name: "msgs", color: col.a, kind: "bar", values: S("msgs") }]} />
        </Tile>
        <Tile title="Orders and fills / min" sub={`${totals.orders} orders · ${totals.fills} fills`}>
          <Series
            from={from}
            to={to}
            fmtV={(v) => Math.round(v).toString()}
            series={[
              { name: "orders", color: col.a, kind: "bar", values: S("orders") },
              { name: "fills", color: col.b, values: S("fills") },
            ]}
          />
        </Tile>
        <Tile title="Risk rejects and ring loss / min" sub={`${totals.rejects} rejects · ${totals.lost} lost`}>
          <Series
            from={from}
            to={to}
            fmtV={(v) => Math.round(v).toString()}
            series={[
              { name: "rejects", color: col.r, kind: "bar", values: S("rejects") },
              { name: "ring loss", color: col.w, values: S("lost") },
            ]}
          />
        </Tile>
        <Tile title="Demo strategy P&L" sub="cash + inventory at mid, after fees" span={2}>
          <Series
            zero={false}
            from={from}
            to={to}
            fmtV={(v) => fmt.usd(v)}
            series={[
              { name: "P&L", color: col.b, values: S("pnl") },
              { name: "fees", color: col.r, values: S("fees", (r) => (r.fees === null ? null : -r.fees)) },
            ]}
          />
        </Tile>
        <Tile title="Position">
          <Series zero={false} from={from} to={to} fmtV={(v) => fmt.qty(v, qd)} series={[{ name: "pos", color: col.v, kind: "step", values: S("pos") }]} />
        </Tile>
        <Tile title="Uplink health" sub="updates / min and longest gap">
          <Series
            from={from}
            to={to}
            fmtV={(v) => Math.round(v).toLocaleString()}
            series={[
              { name: "updates", color: col.a, kind: "bar", values: S("updates") },
              { name: "max gap s", color: col.w, values: S("gap", (r) => r.gap / 1000) },
            ]}
          />
          <p className="note tiny">With no viewers the uplink pauses deltas and sends a snapshot every 30 s, so 30 s gaps then are expected.</p>
        </Tile>
        <Tile title="Fill markouts" sub={`${m?.markouts?.length ?? 0} fills · avg per unit, + = favourable`}>
          <div className="mo">
            {[
              ["Half-spread at fill", mo?.spread],
              ["+1 s", mo?.[1]],
              ["+5 s", mo?.[5]],
              ["+60 s", mo?.[60]],
            ].map(([l, x]) => (
              <div key={l} className="mo-cell">
                <span>{l}</span>
                <b className={x?.n ? (x.avg >= 0 ? "bid" : "ask") : ""}>{x?.n ? usdTicks(x.avg) : "—"}</b>
                <em>{x?.n ? `n=${x.n}` : "no fills yet"}</em>
              </div>
            ))}
          </div>
          <p className="note tiny">Mid move after each engine paper fill, signed by side, in USD per BTC (mid sampled about once a second at the edge). Taker fills pay the half-spread up front; markouts show whether the signal paid for it.</p>
        </Tile>
        <Tile title="Recent engine fills" span={2}>
          {!m?.markouts?.length ? (
            <p className="empty">No marked-out fills yet. A fill is marked out 60 s after it happens.</p>
          ) : (
            <div className="scroll">
              <table>
                <thead>
                  <tr><th>Time</th><th>Side</th><th>Price</th><th>Size</th><th>Liq</th><th>vs mid</th><th>+1 s</th><th>+5 s</th><th>+60 s</th></tr>
                </thead>
                <tbody>
                  {[...m.markouts].reverse().map((x, i) => (
                    <tr key={i}>
                      <td className="muted">{new Date(x.t).toLocaleTimeString("en-GB")}</td>
                      <td className={x.side === "buy" ? "bid" : "ask"}>{x.side}</td>
                      <td>{fmt.price(x.p, pd)}</td>
                      <td>{fmt.qty(x.q, qd)}</td>
                      <td>{x.liq === "M" ? "maker" : "taker"}</td>
                      {[x.spread, x.mo[1], x.mo[5], x.mo[60]].map((v, k) => (
                        <td key={k} className={v === null ? "muted" : v >= 0 ? "bid" : "ask"}>{usdTicks(v)}</td>
                      ))}
                    </tr>
                  ))}
                </tbody>
              </table>
            </div>
          )}
        </Tile>
      </div>
      <p className="note engine-foot">
        Per-minute rows are computed at the Cloudflare edge from the engine's telemetry stream (counter and histogram
        differences), kept for 24 hours. The engine itself also enforces risk limits (size, position, notional, order rate,
        daily loss, kill switch); limit values are visible to the owner on the admin page.
      </p>
    </div>
  );
}

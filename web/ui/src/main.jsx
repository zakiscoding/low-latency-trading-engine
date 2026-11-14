import { StrictMode, useEffect, useMemo, useState } from "react";
import { createRoot } from "react-dom/client";
import { DepthChart, LatencyHist } from "./charts.jsx";
import { fmt, useMarket } from "./market.js";
import { Trade } from "./trade.jsx";
import "./styles.css";

export function PaperBanner() {
  return <div className="paper-banner">PAPER TRADING · SIMULATED FILLS · NO REAL ORDERS ARE EVER SENT</div>;
}

function Header({ page, conn, state }) {
  const up = state?.upstream !== false && state?.stats?.engine_up !== false && !state?.offline;
  const live = conn === "live" && up;
  return (
    <header className="top">
      <h1>Low-latency C++ trading engine</h1>
      <span className="status">
        <span className={"dot " + (live ? "live" : conn === "live" ? "down" : "")} />
        {live ? "live" : conn === "live" ? "engine offline (showing last snapshot)" : conn}
        {state?.meta && <> · {state.meta.venue} {state.meta.product}</>}
      </span>
      <nav>
        <a href="/" className={page === "live" ? "on" : ""}>Live</a>
        <a href="/trade" className={page === "trade" ? "on" : ""}>Paper trade</a>
        <a href="https://github.com/zakiscoding/low-latency-trading-engine">Code</a>
      </nav>
    </header>
  );
}

function Ladder({ bids, asks, meta }) {
  bids = bids ?? [];
  asks = asks ?? [];
  const pd = meta?.price_dec ?? 2,
    qd = meta?.qty_dec ?? 8;
  const rows = 12;
  const a = asks.slice(0, rows).reverse(),
    b = bids.slice(0, rows);
  const maxQ = Math.max(1, ...a.map((l) => l[1]), ...b.map((l) => l[1]));
  const spread = asks[0] && bids[0] ? asks[0][0] - bids[0][0] : null;
  const row = (l, cls, soft) => (
    <tr key={cls + l[0]}>
      <td className={cls}>
        <span>{fmt.price(l[0], pd)}</span>
      </td>
      <td>
        <div className="bar" style={{ width: `${(l[1] / maxQ) * 100}%`, background: soft }} />
        <span>{fmt.qty(l[1], qd)}</span>
      </td>
    </tr>
  );
  return (
    <table className="ladder">
      <thead>
        <tr>
          <th>Price (USD)</th>
          <th>Size ({meta?.product?.split("-")[0] ?? ""})</th>
        </tr>
      </thead>
      <tbody>
        {a.map((l) => row(l, "ask", "var(--ask-soft)"))}
        <tr>
          <td colSpan={2} className="spread">
            {spread !== null ? `spread ${fmt.price(spread, pd)}` : "—"}
          </td>
        </tr>
        {b.map((l) => row(l, "bid", "var(--bid-soft)"))}
      </tbody>
    </table>
  );
}

function Trades({ trades, meta }) {
  const t = [...(trades ?? [])].reverse().slice(0, 40);
  return (
    <div className="trades">
      <table>
        <thead>
          <tr>
            <th>Time</th>
            <th>Price</th>
            <th>Size</th>
          </tr>
        </thead>
        <tbody>
          {t.map((x, i) => (
            <tr key={i}>
              <td className="muted">{fmt.time(x.ts)}</td>
              {/* s = side of the resting order hit: an ask lifted is a buy */}
              <td className={x.s === "a" ? "bid" : "ask"}>{fmt.price(x.p, meta?.price_dec)}</td>
              <td>{fmt.qty(x.q, meta?.qty_dec)}</td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}

function useResults() {
  const [res, setRes] = useState([]);
  useEffect(() => {
    fetch("/results/index.json")
      .then((r) => r.json())
      .then((files) => Promise.all(files.map((f) => fetch("/results/" + f).then((r) => r.json()).then((j) => ({ file: f, ...j })))))
      .then(setRes)
      .catch(() => {});
  }, []);
  return res;
}

function BenchTables() {
  const res = useResults();
  const ladder = res.filter((r) => r.bench === "optimization_ladder");
  const pipe = []; // raw per-repeat runs are summarized in optimization_ladder.json
  const udp = res.filter((r) => r.bench === "udp_tick_to_trade");
  if (!res.length) return <p className="note">No benchmark results bundled.</p>;
  return (
    <>
      {ladder.map((l) => (
        <div key={l.file} style={{ marginBottom: 16 }}>
          <p className="note">
            {l.title} — {l.machine}
          </p>
          <table>
            <thead>
              <tr>
                <th>Step</th>
                <th>What changed</th>
                <th>p50</th>
                <th>p99</th>
                <th>p99.9</th>
                <th>Throughput</th>
              </tr>
            </thead>
            <tbody>
              {l.rows.map((r) => (
                <tr key={r.step}>
                  <td>{r.step}</td>
                  <td style={{ textAlign: "left", whiteSpace: "normal" }}>{r.change}</td>
                  <td>{fmt.ns(r.p50)}</td>
                  <td>{fmt.ns(r.p99)}</td>
                  <td>{fmt.ns(r.p999)}</td>
                  <td>{r.throughput ?? ""}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      ))}
      {pipe.map((p) => (
        <div key={p.file} style={{ marginBottom: 16 }}>
          <p className="note">
            In-process pipeline, {p.messages.toLocaleString()} {p.source} ITCH messages, {p.cpu} ({p.file.split("/")[0]})
          </p>
          <table>
            <thead>
              <tr>
                <th>Variant</th>
                <th>Msg/s</th>
                <th>book p50</th>
                <th>book p99</th>
                <th>tick→trade p50</th>
                <th>p99</th>
                <th>p99.9</th>
              </tr>
            </thead>
            <tbody>
              {p.variants.map((v) => (
                <tr key={v.name}>
                  <td>{v.name}</td>
                  <td>{(v.msgs_per_sec / 1e6).toFixed(2)}M</td>
                  <td>{fmt.ns(v.stages.book.p50)}</td>
                  <td>{fmt.ns(v.stages.book.p99)}</td>
                  <td>{fmt.ns(v.stages.tick_to_trade.p50)}</td>
                  <td>{fmt.ns(v.stages.tick_to_trade.p99)}</td>
                  <td>{fmt.ns(v.stages.tick_to_trade.p999)}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      ))}
      {udp.length > 0 && (
        <>
          <p className="note">
            UDP replay on loopback, kernel RX timestamp → order send() returned. Laptop numbers are inside a Hyper-V VM
            (Docker Desktop) and are dominated by VM scheduling; clean runs on the tuned Ubuntu box are pending.
          </p>
          <table>
            <thead>
              <tr>
                <th>Run</th>
                <th>RX mode</th>
                <th>Orders</th>
                <th>p50</th>
                <th>p99</th>
                <th>p99.9</th>
                <th>max</th>
              </tr>
            </thead>
            <tbody>
              {udp.filter((u) => u.kernel_rx_to_send).map((u) => (
                <tr key={u.file}>
                  <td>{u.label || u.file}</td>
                  <td>{u.rx}</td>
                  <td>{u.orders}</td>
                  <td>{fmt.ns(u.kernel_rx_to_send.p50)}</td>
                  <td>{fmt.ns(u.kernel_rx_to_send.p99)}</td>
                  <td>{fmt.ns(u.kernel_rx_to_send.p999)}</td>
                  <td>{fmt.ns(u.kernel_rx_to_send.max)}</td>
                </tr>
              ))}
            </tbody>
          </table>
        </>
      )}
    </>
  );
}

function Leaderboard() {
  const [lb, setLb] = useState(null);
  useEffect(() => {
    const load = () => fetch("/api/leaderboard").then((r) => r.json()).then(setLb).catch(() => {});
    load();
    const t = setInterval(load, 30000);
    return () => clearInterval(t);
  }, []);
  return (
    <>
      <p className="note">Week starting {lb?.week ?? "—"} (UTC). Paper money only; resets Monday.</p>
      {!lb?.rows?.length ? (
        <p className="note">No paper trades yet this week. <a href="/trade">Be the first.</a></p>
      ) : (
        <table>
          <thead>
            <tr>
              <th>#</th>
              <th>Trader</th>
              <th>Paper P&L</th>
            </tr>
          </thead>
          <tbody>
            {lb.rows.map((r) => (
              <tr key={r.rank}>
                <td>{r.rank}</td>
                <td>{r.name}</td>
                <td className={Number(r.pnl_micros) >= 0 ? "bid" : "ask"}>{fmt.usd(r.pnl_micros)}</td>
              </tr>
            ))}
          </tbody>
        </table>
      )}
    </>
  );
}

function Live({ state }) {
  const meta = state?.meta;
  const t2t = state?.latency?.tick_to_trade;
  const wire = state?.latency?.kernel_rx_to_send;
  const st = state?.stats ?? {};
  const pnl = useMemo(() => {
    if (st.cash_micros === undefined) return null;
    // Demo strategy P&L: cash + inventory marked at mid.
    const pos = st.position ?? 0;
    const mid = st.mark_mid ?? 0;
    return st.cash_micros + (pos * mid * 1e6) / 10 ** ((meta?.price_dec ?? 2) + (meta?.qty_dec ?? 8));
  }, [st.cash_micros, st.position, st.mark_mid, meta]);
  return (
    <div className="grid">
      <section className="card span-4">
        <h2>
          Order book <small>{meta?.attribution}</small>
        </h2>
        <Ladder bids={state?.bids} asks={state?.asks} meta={meta} />
      </section>
      <section className="card span-5">
        <h2>Depth</h2>
        <DepthChart bids={state?.bids} asks={state?.asks} priceDec={meta?.price_dec} qtyDec={meta?.qty_dec} />
        <h2 style={{ marginTop: 16 }}>
          Engine tick-to-trade, live <small>{t2t?.n ? `${t2t.n.toLocaleString()} orders` : ""}</small>
        </h2>
        <LatencyHist buckets={t2t?.buckets} marks={t2t ?? {}} />
        <p className="note">
          In-process: last byte of the decrypted message available → order <code>send()</code> returned, measured with{" "}
          <code>rdtsc</code> on the engine&apos;s pinned thread.
        </p>
        {wire?.n > 0 && (
          <>
            <h2 style={{ marginTop: 12 }}>Kernel RX → send, live</h2>
            <LatencyHist buckets={wire.buckets} marks={wire} color="#8f7cf0" height={150} />
            <p className="note">
              From the TCP socket&apos;s <code>SO_TIMESTAMPING</code> RX stamp. Includes TLS decryption and the WebSocket
              framing of a public internet feed. Network latency to the exchange (tens of ms from a home connection) is
              not included and dwarfs both numbers.
            </p>
          </>
        )}
      </section>
      <section className="card span-3">
        <h2>Trades</h2>
        <Trades trades={state?.trades} meta={meta} />
      </section>
      <section className="card span-12">
        <h2>Demo strategy (paper) <small>not expected to be profitable</small></h2>
        <div className="kpis">
          <div className="kpi"><div className="v">{st.messages?.toLocaleString() ?? "—"}</div><div className="l">feed messages</div></div>
          <div className="kpi"><div className="v">{st.orders ?? "—"}</div><div className="l">paper orders</div></div>
          <div className="kpi"><div className="v">{st.fills ?? "—"}</div><div className="l">simulated fills</div></div>
          <div className="kpi"><div className="v">{st.position !== undefined ? fmt.qty(st.position, meta?.qty_dec) : "—"}</div><div className="l">position</div></div>
          <div className="kpi"><div className={"v " + (pnl >= 0 ? "bid" : "ask")}>{pnl !== null ? fmt.usd(pnl) : "—"}</div><div className="l">P&L incl. fees</div></div>
          <div className="kpi"><div className="v">{st.fees_micros !== undefined ? fmt.usd(st.fees_micros) : "—"}</div><div className="l">fees paid</div></div>
          <div className="kpi"><div className={"v " + (st.kill ? "ask" : "")}>{st.kill ? "KILLED" : st.trading === false ? "stopped" : "running"}</div><div className="l">strategy</div></div>
        </div>
      </section>
      <section className="card span-8">
        <h2>Benchmarks: before / after</h2>
        <BenchTables />
      </section>
      <section className="card span-4">
        <h2>Weekly paper P&L leaderboard</h2>
        <Leaderboard />
      </section>
    </div>
  );
}

function App() {
  const page = location.pathname.startsWith("/trade") ? "trade" : "live";
  const { state, conn } = useMarket();
  return (
    <>
      <PaperBanner />
      <Header page={page} conn={conn} state={state} />
      <main>{page === "trade" ? <Trade state={state} /> : <Live state={state} />}</main>
      <footer>
        Paper trading only: no brokerage connection, no real orders. Live crypto data from Bitstamp's public feed.
        Viewers are served from Cloudflare's edge, never from the machine running the engine.
      </footer>
    </>
  );
}

createRoot(document.getElementById("root")).render(
  <StrictMode>
    <App />
  </StrictMode>,
);

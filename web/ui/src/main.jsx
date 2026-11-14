// The public site: a trading-terminal layout over the engine's live stream.
//   top bar     symbol, last price, rolling change / high / low / volume, status, tabs
//   center      candlestick chart built from the engine's trades
//   right       order book (click a price to fill the ticket), recent trades
//   right rail  paper order ticket (or sign-in when signed out)
//   bottom      positions, orders, history, fills, engine latency, strategy, benchmarks, leaderboard
// On phones the same panels become tabs under the chart.
import { StrictMode, useEffect, useMemo, useRef, useState } from "react";
import { createRoot } from "react-dom/client";
import { aggregate, rollingStats } from "./candles.js";
import { CandleChart } from "./chart.jsx";
import { EnginePage } from "./engine.jsx";
import { DepthChart, LatencyHist } from "./charts.jsx";
import { fmt, useMarket } from "./market.js";
import { AccountSettings, AccountSummary, Fills, OpenOrders, OrderHistory, Positions, SignInPanel, Ticket, useAccount } from "./trade.jsx";
import "./styles.css";

export function PaperBanner() {
  return <div className="paper-banner">PAPER TRADING · SIMULATED FILLS · NO REAL ORDERS ARE EVER SENT</div>;
}

const INTERVALS = [
  ["1m", 1],
  ["5m", 5],
  ["15m", 15],
  ["1h", 60],
];

function span(ms) {
  const h = ms / 3_600_000;
  return h >= 23 ? "24h" : h >= 1 ? `${Math.floor(h)}h` : `${Math.max(1, Math.round(ms / 60_000))}m`;
}

// Price that flashes green/red when it changes.
function Flash({ value, children, className = "" }) {
  const prev = useRef(value);
  const [dir, setDir] = useState("");
  const [n, setN] = useState(0);
  useEffect(() => {
    if (value === undefined || prev.current === undefined) {
      prev.current = value;
      return;
    }
    if (value !== prev.current) {
      setDir(value > prev.current ? "up" : "down");
      setN((x) => x + 1);
      prev.current = value;
    }
  }, [value]);
  return (
    <span key={n} className={`${className} ${dir} ${n ? "flash-" + dir : ""}`}>
      {children}
    </span>
  );
}

function TopBar({ page, conn, state, stats }) {
  const up = state?.upstream !== false && state?.stats?.engine_up !== false && !state?.offline;
  const live = conn === "live" && up;
  const meta = state?.meta;
  const pd = meta?.price_dec ?? 2;
  const lastTrade = state?.trades?.[state.trades.length - 1];
  const last = lastTrade?.p ?? stats?.last;
  const label = stats ? span(stats.spanMs) : "24h";
  return (
    <header className="topbar">
      <div className="sym">
        <span className="pair">{meta?.product?.replace("-", " / ") ?? "BTC / USD"}</span>
        <span className="venue">{meta?.venue ?? "bitstamp"} · paper</span>
      </div>
      <div className="last">
        <Flash value={last} className="px">{last ? fmt.price(last, pd) : "—"}</Flash>
        {stats && (
          <span className={stats.change >= 0 ? "bid" : "ask"}>
            {stats.change >= 0 ? "+" : ""}
            {fmt.price(stats.change, pd)} ({stats.pct >= 0 ? "+" : ""}
            {stats.pct.toFixed(2)}%)
          </span>
        )}
      </div>
      <dl className="stats">
        <div><dt>{label} change</dt><dd className={stats && stats.change < 0 ? "ask" : "bid"}>{stats ? `${stats.pct >= 0 ? "+" : ""}${stats.pct.toFixed(2)}%` : "—"}</dd></div>
        <div><dt>{label} high</dt><dd>{stats ? fmt.price(stats.hi, pd) : "—"}</dd></div>
        <div><dt>{label} low</dt><dd>{stats ? fmt.price(stats.lo, pd) : "—"}</dd></div>
        <div><dt>{label} volume</dt><dd>{stats ? `${(stats.vol / 10 ** (meta?.qty_dec ?? 8)).toFixed(2)} ${meta?.product?.split("-")[0] ?? ""}` : "—"}</dd></div>
      </dl>
      <span className="status" title={live ? "streaming from the engine" : "not streaming"}>
        <span className={"dot " + (live ? "live" : conn === "live" ? "down" : "")} />
        {live ? "Live" : conn === "live" ? "Engine offline" : conn === "connecting" ? "Connecting" : "Reconnecting"}
      </span>
      <nav>
        <a href="/" className={page === "live" ? "on" : ""}>Live</a>
        <a href="/trade" className={page === "trade" ? "on" : ""}>Paper trade</a>
        <a href="/engine" className={page === "engine" ? "on" : ""}>Engine</a>
        <a href="https://github.com/zakiscoding/low-latency-trading-engine">Code</a>
      </nav>
    </header>
  );
}

function OrderBook({ state, onPrice }) {
  const meta = state?.meta;
  const pd = meta?.price_dec ?? 2,
    qd = meta?.qty_dec ?? 8;
  const rows = 14;
  const bids = (state?.bids ?? []).slice(0, rows);
  const asks = (state?.asks ?? []).slice(0, rows);
  // Cumulative depth bars, like most terminals.
  const cum = (lv) => {
    let s = 0;
    return lv.map(([p, q]) => [p, q, (s += q)]);
  };
  const cb = cum(bids),
    ca = cum(asks);
  const maxC = Math.max(1, cb[cb.length - 1]?.[2] ?? 0, ca[ca.length - 1]?.[2] ?? 0);
  const spread = asks[0] && bids[0] ? asks[0][0] - bids[0][0] : null;
  const mid = asks[0] && bids[0] ? (asks[0][0] + bids[0][0]) / 2 : null;
  const base = meta?.product?.split("-")[0] ?? "";
  const row = ([p, q, c], side) => (
    <div key={side + p} className={"lvl " + side} onClick={() => onPrice((p / 10 ** pd).toFixed(pd))} title="Click to use this price">
      <div className="depth" style={{ width: `${(c / maxC) * 100}%` }} />
      <span className="p">{fmt.price(p, pd)}</span>
      <span>{fmt.qty(q, qd)}</span>
      <span className="muted">{fmt.qty(c, qd)}</span>
    </div>
  );
  return (
    <div className="book">
      <div className="lvl head">
        <span>Price (USD)</span>
        <span>Size ({base})</span>
        <span>Total</span>
      </div>
      <div className="asks">{[...ca].reverse().map((l) => row(l, "ask"))}</div>
      <div className="midrow">
        <Flash value={mid} className="mid">{mid ? fmt.price(mid, pd) : "—"}</Flash>
        <span className="muted">spread {spread !== null ? `${fmt.price(spread, pd)} (${((spread / mid) * 1e4).toFixed(2)} bp)` : "—"}</span>
      </div>
      <div className="bids">{cb.map((l) => row(l, "bid"))}</div>
    </div>
  );
}

function TradeTape({ state }) {
  const meta = state?.meta;
  const t = [...(state?.trades ?? [])].reverse().slice(0, 60);
  const base = meta?.product?.split("-")[0] ?? "";
  return (
    <div className="tape">
      <div className="lvl head">
        <span>Price (USD)</span>
        <span>Size ({base})</span>
        <span>Time</span>
      </div>
      <div className="tape-rows">
        {t.map((x, i) => (
          <div className={"lvl" + (i === 0 ? " fresh" : "")} key={`${x.ts}-${x.p}-${x.q}-${i}`}>
            {/* s = side of the resting order hit: an ask lifted is a buy */}
            <span className={x.s === "a" ? "bid" : "ask"}>{fmt.price(x.p, meta?.price_dec)}</span>
            <span>{fmt.qty(x.q, meta?.qty_dec)}</span>
            <span className="muted">{fmt.time(x.ts)}</span>
          </div>
        ))}
        {!t.length && <p className="empty">Waiting for trades…</p>}
      </div>
    </div>
  );
}

function LatencyPanel({ state }) {
  const t2t = state?.latency?.tick_to_trade;
  const wire = state?.latency?.kernel_rx_to_send;
  const meta = state?.meta;
  if (!t2t?.n) {
    return (
      <div className="explain">
        <p>
          <b>No tick-to-trade samples in this session yet.</b> A sample is recorded only when the demo strategy sends a
          paper order. It trades on top-of-book imbalance, so in a quiet market it can go minutes without an order, and the
          histogram starts empty whenever the engine restarts.
        </p>
        <p className="note">
          What it measures: last byte of the decrypted market-data message available → order <code>send()</code> returned,
          timed with the CPU's cycle counter on the engine's hot thread. Replay benchmarks are under Benchmarks.
        </p>
        {meta?.note && <p className="note">{meta.note}</p>}
      </div>
    );
  }
  return (
    <div className="lat">
      <div>
        <div className="sub">
          Engine tick-to-trade, live <span className="muted">{t2t.n.toLocaleString()} orders · p50 {fmt.ns(t2t.p50)} · p99 {fmt.ns(t2t.p99)}</span>
        </div>
        <LatencyHist buckets={t2t.buckets} marks={t2t} height={170} />
        <p className="note">
          In-process: decrypted message available → order <code>send()</code> returned, measured with <code>rdtsc</code>.
          {meta?.note ? ` ${meta.note}` : ""}
        </p>
      </div>
      {wire?.n > 0 && (
        <div>
          <div className="sub">Kernel RX → send, live</div>
          <LatencyHist buckets={wire.buckets} marks={wire} color="#8f7cf0" height={170} />
          <p className="note">From the socket's SO_TIMESTAMPING RX stamp; includes TLS and WebSocket framing.</p>
        </div>
      )}
    </div>
  );
}

function StrategyPanel({ state }) {
  const meta = state?.meta;
  const st = state?.stats ?? {};
  const pnl = useMemo(() => {
    if (st.cash_micros === undefined) return null;
    const pos = st.position ?? 0;
    const mid = st.mark_mid ?? 0;
    return st.cash_micros + (pos * mid * 1e6) / 10 ** ((meta?.price_dec ?? 2) + (meta?.qty_dec ?? 8));
  }, [st.cash_micros, st.position, st.mark_mid, meta]);
  const k = (v, l, cls = "") => (
    <div className="kpi"><div className={"v " + cls}>{v}</div><div className="l">{l}</div></div>
  );
  return (
    <div>
      <div className="kpis">
        {k(st.messages?.toLocaleString() ?? "—", "feed messages")}
        {k(st.orders ?? "—", "engine paper orders")}
        {k(st.fills ?? "—", "simulated fills")}
        {k(st.position !== undefined ? fmt.qty(st.position, meta?.qty_dec) : "—", "position")}
        {k(pnl !== null ? fmt.usd(pnl) : "—", "P&L incl. fees", pnl >= 0 ? "bid" : "ask")}
        {k(st.fees_micros !== undefined ? fmt.usd(st.fees_micros) : "—", "fees paid")}
        {k(st.kill ? "KILLED" : st.trading === false ? "stopped" : "running", "strategy", st.kill ? "ask" : "")}
      </div>
      <p className="note">The C++ engine's own demo strategy (top-of-book imbalance taker) on the paper exchange. Not expected to be profitable.</p>
      <div className="sub" style={{ marginTop: 10 }}>Depth</div>
      <DepthChart bids={state?.bids} asks={state?.asks} priceDec={meta?.price_dec} qtyDec={meta?.qty_dec} height={160} />
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

function Benchmarks() {
  const res = useResults();
  const ladder = res.filter((r) => r.bench === "optimization_ladder");
  const udp = res.filter((r) => r.bench === "udp_tick_to_trade" && r.kernel_rx_to_send);
  if (!res.length) return <p className="empty">Loading benchmark results…</p>;
  return (
    <div className="bench">
      {ladder.map((l) => (
        <div key={l.file}>
          <div className="sub">{l.title} <span className="muted">· {l.machine}</span></div>
          <table>
            <thead><tr><th>Step</th><th>What changed</th><th>p50</th><th>p99</th><th>p99.9</th><th>Throughput</th></tr></thead>
            <tbody>
              {l.rows.map((r) => (
                <tr key={r.step}>
                  <td>{r.step}</td>
                  <td className="wrap">{r.change}</td>
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
      {udp.length > 0 && (
        <div>
          <div className="sub">UDP replay: kernel RX stamp → order <code>send()</code> <span className="muted">· laptop VM, not a clean benchmark</span></div>
          <table>
            <thead><tr><th>Run</th><th>RX mode</th><th>Orders</th><th>p50</th><th>p99</th><th>p99.9</th><th>max</th></tr></thead>
            <tbody>
              {udp.map((u) => (
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
        </div>
      )}
    </div>
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
  if (!lb?.rows?.length)
    return <p className="empty">No paper trades yet this week (week of {lb?.week ?? "—"}, UTC). <a href="/trade">Be the first.</a></p>;
  return (
    <table>
      <thead><tr><th>#</th><th>Trader</th><th>Paper P&L this week</th></tr></thead>
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
  );
}

function NeedSignIn() {
  return <p className="empty">Sign in from the order ticket to see your paper positions and orders.</p>;
}

function BottomPanel({ state, acc, tab, setTab, only }) {
  const signed = acc.data?.signedIn;
  const n = signed ? acc.data.account : null;
  const tabs = [
    ["positions", "Positions"],
    ["open", `Open orders${n?.open.length ? ` (${n.open.length})` : ""}`],
    ["history", "Order history"],
    ["fills", "Fills"],
    ["latency", "Engine latency"],
    ["strategy", "Demo strategy"],
    ["bench", "Benchmarks"],
    ["leaders", "Leaderboard"],
    ...(signed ? [["account", "Account"]] : []),
  ].filter(([k]) => !only || only.includes(k));
  if (only && !only.includes(tab)) tab = only[0];
  let body;
  const acctTab = ["positions", "open", "history", "fills", "account"].includes(tab);
  if (acctTab && !signed) body = acc.data ? <NeedSignIn /> : <p className="empty">Loading…</p>;
  else if (tab === "positions") body = <Positions acc={acc} state={state} />;
  else if (tab === "open") body = <OpenOrders acc={acc} />;
  else if (tab === "history") body = <OrderHistory acc={acc} />;
  else if (tab === "fills") body = <Fills acc={acc} />;
  else if (tab === "account") body = <AccountSettings acc={acc} />;
  else if (tab === "latency") body = <LatencyPanel state={state} />;
  else if (tab === "strategy") body = <StrategyPanel state={state} />;
  else if (tab === "bench") body = <Benchmarks />;
  else body = <Leaderboard />;
  return (
    <section className="panel bottom">
      <div className="ptabs">
        {tabs.map(([k, l]) => (
          <button key={k} className={tab === k ? "on" : ""} onClick={() => setTab(k)}>{l}</button>
        ))}
      </div>
      <div className="pbody">{body}</div>
    </section>
  );
}

function Rail({ state, acc, price, setPrice }) {
  if (!acc.data) return <section className="panel rail"><p className="empty">Loading…</p></section>;
  return (
    <section className="panel rail">
      <div className="ph">Order ticket <span className="tag warn">paper</span></div>
      {acc.data.signedIn ? (
        <>
          <AccountSummary acc={acc} />
          <Ticket acc={acc} state={state} price={price} setPrice={setPrice} />
        </>
      ) : (
        <SignInPanel config={acc.config} />
      )}
    </section>
  );
}

function useNarrow(px = 760) {
  const [n, setN] = useState(() => window.matchMedia(`(max-width: ${px}px)`).matches);
  useEffect(() => {
    const m = window.matchMedia(`(max-width: ${px}px)`);
    const f = () => setN(m.matches);
    m.addEventListener("change", f);
    return () => m.removeEventListener("change", f);
  }, [px]);
  return n;
}

function Terminal({ page }) {
  const { state, conn } = useMarket();
  const acc = useAccount();
  const [tf, setTf] = useState(() => Number(localStorageGet("lle.tf")) || 1);
  const [price, setPrice] = useState("");
  const [tab, setTab] = useState(null);
  const [mtab, setMtab] = useState(page === "trade" ? "order" : "book");
  const narrow = useNarrow();
  const bars1m = state?.candles?.bars ?? [];
  const bars = useMemo(() => aggregate(bars1m, tf), [bars1m, tf]);
  const stats = useMemo(() => rollingStats(bars1m), [bars1m]);
  const markers = useMemo(
    () => (acc.data?.signedIn ? acc.data.account.fills.map((f) => ({ t: f.ts, side: f.side, price: Number(f.price) })) : []),
    [acc.data],
  );
  // Default bottom tab: the account when signed in, else the latency view.
  const btab = tab ?? (acc.data?.signedIn ? "positions" : "latency");
  const pickPrice = (p) => {
    setPrice(p);
    if (narrow) setMtab("order");
  };
  useEffect(() => localStorageSet("lle.tf", String(tf)), [tf]);

  const chart = (
    <section className="panel chart">
      <div className="ph">
        <div className="intervals">
          {INTERVALS.map(([l, m]) => (
            <button key={l} className={tf === m ? "on" : ""} onClick={() => setTf(m)}>{l}</button>
          ))}
        </div>
        <span className="muted hint">Candles from the trades the engine streams · scroll to zoom, drag to pan</span>
      </div>
      <CandleChart bars={bars} tfMin={tf} priceDec={state?.meta?.price_dec} qtyDec={state?.meta?.qty_dec} markers={markers} symbol={state?.meta?.product ?? ""} />
    </section>
  );
  const book = (
    <section className="panel bookp">
      <div className="ph">Order book <span className="muted">{state?.meta?.attribution ?? ""}</span></div>
      <OrderBook state={state} onPrice={pickPrice} />
    </section>
  );
  const tape = (
    <section className="panel tapep">
      <div className="ph">Recent trades</div>
      <TradeTape state={state} />
    </section>
  );
  const rail = <Rail state={state} acc={acc} price={price} setPrice={setPrice} />;

  return (
    <>
      <PaperBanner />
      <TopBar page={page} conn={conn} state={state} stats={stats} />
      {page === "engine" ? (
        <main className="enginep">
          <EnginePage state={state} />
        </main>
      ) : narrow ? (
        <main className="mobile">
          {chart}
          <div className="mtabs">
            {[
              ["book", "Book"],
              ["trades", "Trades"],
              ["order", "Order"],
              ["positions", "Positions"],
              ["more", "More"],
            ].map(([k, l]) => (
              <button key={k} className={mtab === k ? "on" : ""} onClick={() => setMtab(k)}>{l}</button>
            ))}
          </div>
          {mtab === "book" && book}
          {mtab === "trades" && tape}
          {mtab === "order" && rail}
          {mtab === "positions" && <BottomPanel state={state} acc={acc} tab={btab} setTab={setTab} only={["positions", "open", "history", "fills", "account"]} />}
          {mtab === "more" && <BottomPanel state={state} acc={acc} tab={btab} setTab={setTab} only={["latency", "strategy", "bench", "leaders"]} />}
        </main>
      ) : (
        <main className="term">
          {chart}
          <div className="side">
            {book}
            {tape}
          </div>
          {rail}
          <BottomPanel state={state} acc={acc} tab={btab} setTab={setTab} />
        </main>
      )}
      <footer>
        Paper trading only: no brokerage connection, no real orders. Live crypto data from Bitstamp&apos;s public feed.
        Viewers are served from Cloudflare&apos;s edge, never from the machine running the engine.
      </footer>
    </>
  );
}

function localStorageGet(k) {
  try {
    return localStorage.getItem(k);
  } catch {
    return null;
  }
}
function localStorageSet(k, v) {
  try {
    localStorage.setItem(k, v);
  } catch {}
}

function App() {
  const path = location.pathname;
  const page = path.startsWith("/trade") ? "trade" : path.startsWith("/engine") ? "engine" : "live";
  return <Terminal page={page} />;
}

createRoot(document.getElementById("root")).render(
  <StrictMode>
    <App />
  </StrictMode>,
);

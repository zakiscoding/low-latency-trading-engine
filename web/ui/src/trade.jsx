// Signed-in paper trading. Every account is its own Durable Object simulator on
// Cloudflare: it reads the public book from the edge and never reaches the engine.
import { useCallback, useEffect, useRef, useState } from "react";
import { fmt } from "./market.js";

const QTY = 1e8; // BTC units in the account API
const TAKER = 0.004,
  MAKER = 0.003;

function useTurnstile(siteKey, active = true) {
  const ref = useRef(null);
  const [token, setToken] = useState("");
  useEffect(() => {
    if (!siteKey || !ref.current || !active) return;
    let id = null;
    const render = () => {
      if (ref.current && !ref.current.childElementCount)
        id = window.turnstile?.render(ref.current, { sitekey: siteKey, theme: "dark", callback: setToken, "expired-callback": () => setToken("") });
    };
    if (window.turnstile) render();
    else {
      let s = document.querySelector("script[data-turnstile]");
      if (!s) {
        s = document.createElement("script");
        s.src = "https://challenges.cloudflare.com/turnstile/v0/api.js";
        s.async = true;
        s.dataset.turnstile = "1";
        document.head.appendChild(s);
      }
      s.addEventListener("load", render);
    }
    return () => {
      if (id !== null && id !== undefined) window.turnstile?.remove?.(id);
    };
  }, [siteKey, active]);
  return { ref, token };
}

async function api(path, body) {
  const r = await fetch(path, body === undefined ? {} : { method: "POST", headers: { "content-type": "application/json" }, body: JSON.stringify(body) });
  const j = await r.json().catch(() => ({ error: `HTTP ${r.status}` }));
  return { status: r.status, ...j };
}

// Account state for the whole terminal: null while loading, {signedIn:false} or
// the account view. Polls every 5 s while the tab is visible.
export function useAccount() {
  const [config, setConfig] = useState(null);
  const [data, setData] = useState(null);
  const [msg, setMsg] = useState(null);
  const refresh = useCallback(async () => {
    if (document.hidden) return;
    const r = await api("/api/account");
    if (r.status === 401) setData({ signedIn: false });
    else if (r.account) setData({ signedIn: true, ...r });
  }, []);
  useEffect(() => {
    fetch("/api/config").then((r) => r.json()).then(setConfig).catch(() => setConfig({}));
    refresh();
    const t = setInterval(refresh, 5000);
    return () => clearInterval(t);
  }, [refresh]);
  const act = useCallback(async (path, body, label) => {
    const r = await api(path, body);
    const err = r.error || r.result?.error;
    if (err) setMsg({ err });
    else if (path === "/api/order") {
      const x = r.result;
      setMsg({ ok: `Paper order #${x.id}: filled ${fmt.qty(x.filled)}${Number(x.rested) ? `, resting ${fmt.qty(x.rested)}` : ""}` });
    } else setMsg({ ok: label });
    if (r.account) setData({ signedIn: true, ...r });
    if (path === "/api/delete" && r.deleted) location.href = "/";
    return r;
  }, []);
  return { config, data, msg, setMsg, act, refresh };
}

export function SignInPanel({ config }) {
  const { ref, token } = useTurnstile(config?.turnstileSiteKey);
  const providers = config?.providers ?? [];
  return (
    <div className="signin">
      <div className="signin-title">Sign in to paper trade</div>
      <p className="note">
        Get a sandboxed <b>paper</b> account with $100,000 of pretend money, filled against the live Bitstamp book with
        the same queue-position model the C++ simulator uses. Nothing is ever sent to an exchange.
      </p>
      {!providers.length && config && <p className="err">Sign-in isn&apos;t configured on this deployment yet.</p>}
      <div ref={ref} className="turnstile" />
      <div className="signin-buttons">
        {providers.map((p) => (
          <form key={p} method="POST" action={`/auth/${p}/start`}>
            <input type="hidden" name="cf-turnstile-response" value={token} />
            <button className="primary wide" disabled={!!config?.turnstileSiteKey && !token}>
              Continue with {p === "github" ? "GitHub" : "Google"}
            </button>
          </form>
        ))}
      </div>
      <Privacy />
    </div>
  );
}

function Privacy() {
  return (
    <details className="privacy">
      <summary className="note">Privacy</summary>
      <p className="note">
        No passwords. Sign-in is through GitHub or Google; the site keeps a hashed account id, your public handle
        (GitHub login, or a Google first name plus 4 characters), and your paper balances and orders. Access tokens are
        used once to read the profile and then discarded; email addresses are never requested or stored. The weekly
        leaderboard shows your handle and paper P&L; you can hide yourself or delete your account (and its leaderboard
        row) under Account. A session cookie keeps you signed in for 7 days. Cloudflare processes requests as the host.
      </p>
    </details>
  );
}

const usd = (v) => (v < 0 ? "-$" : "$") + Math.abs(v).toLocaleString("en-US", { minimumFractionDigits: 2, maximumFractionDigits: 2 });

// The order ticket. `price` (dollars, string) can be set from the order book.
export function Ticket({ acc, state, price, setPrice }) {
  const [side, setSide] = useState("buy");
  const [type, setType] = useState("limit");
  const [qty, setQty] = useState("0.01");
  const meta = state?.meta;
  const pd = meta?.price_dec ?? 2;
  const bestBid = state?.bids?.[0]?.[0],
    bestAsk = state?.asks?.[0]?.[0];
  const a = acc.data.account;
  const cash = Number(a.cash_micros) / 1e6,
    btc = Number(a.btc) / QTY;
  // Reserved by resting orders, as the simulator counts it.
  let resCash = 0,
    resBtc = 0;
  for (const o of a.open) {
    const n = (Number(o.price) / 100) * (Number(o.qty) / QTY);
    if (o.side === "buy") resCash += n * (1 + MAKER);
    else resBtc += Number(o.qty) / QTY;
  }
  const availCash = cash - resCash,
    availBtc = btc - resBtc;
  const touch = (side === "buy" ? bestAsk : bestBid) ?? 0;
  const px = type === "market" ? touch / 10 ** pd : Number(price);
  const q = Number(qty);
  const notional = px > 0 && q > 0 ? px * q : 0;
  const fee = notional * (type === "market" ? TAKER : MAKER);

  const setPct = (pct) => {
    const ref = px > 0 ? px : (bestAsk ?? 0) / 10 ** pd;
    if (!(ref > 0)) return;
    let v = side === "buy" ? (availCash * pct) / (ref * (1 + TAKER)) : availBtc * pct;
    v = Math.min(0.5, Math.floor(v * 1e4) / 1e4);
    setQty(v > 0 ? v.toFixed(4) : "0");
  };
  const submit = (e) => {
    e.preventDefault();
    const body = type === "market" ? { side, type, qty } : { side, type, qty, price: String(price) };
    acc.act("/api/order", body);
  };
  return (
    <form className="ticket" onSubmit={submit}>
      <div className="seg">
        <button type="button" className={side === "buy" ? "on buy" : ""} onClick={() => setSide("buy")}>Buy</button>
        <button type="button" className={side === "sell" ? "on sell" : ""} onClick={() => setSide("sell")}>Sell</button>
      </div>
      <div className="seg small">
        <button type="button" className={type === "limit" ? "on" : ""} onClick={() => setType("limit")}>Limit</button>
        <button type="button" className={type === "market" ? "on" : ""} onClick={() => setType("market")}>Market</button>
      </div>
      {type === "limit" ? (
        <label className="field">
          <span>Price</span>
          <input value={price} onChange={(e) => setPrice(e.target.value)} inputMode="decimal" placeholder={bestBid ? (bestBid / 10 ** pd).toFixed(pd) : ""} />
          <em>USD</em>
        </label>
      ) : (
        <div className="field ro">
          <span>Price</span>
          <b>Market</b>
          <em>{touch ? `~${fmt.price(touch, pd)}` : ""}</em>
        </div>
      )}
      {type === "limit" && bestBid && bestAsk && (
        <div className="quick">
          <button type="button" onClick={() => setPrice((bestBid / 10 ** pd).toFixed(pd))}>Bid</button>
          <button type="button" onClick={() => setPrice(((bestBid + bestAsk) / 2 / 10 ** pd).toFixed(pd))}>Mid</button>
          <button type="button" onClick={() => setPrice((bestAsk / 10 ** pd).toFixed(pd))}>Ask</button>
        </div>
      )}
      <label className="field">
        <span>Size</span>
        <input value={qty} onChange={(e) => setQty(e.target.value)} inputMode="decimal" />
        <em>BTC</em>
      </label>
      <div className="quick">
        {[0.25, 0.5, 0.75, 1].map((p) => (
          <button type="button" key={p} onClick={() => setPct(p)}>{p * 100}%</button>
        ))}
      </div>
      <dl className="est">
        <dt>Est. {side === "buy" ? "cost" : "proceeds"}</dt>
        <dd>{notional ? usd(side === "buy" ? notional + fee : notional - fee) : "—"}</dd>
        <dt>Fee ({type === "market" ? "taker 0.40%" : "maker 0.30%*"})</dt>
        <dd>{notional ? usd(fee) : "—"}</dd>
        <dt>Available</dt>
        <dd>{side === "buy" ? usd(availCash) : `${availBtc.toFixed(6)} BTC`}</dd>
      </dl>
      <button className={"wide " + side}>
        {side === "buy" ? "Buy" : "Sell"} BTC <small>(paper)</small>
      </button>
      {acc.msg?.err && <div className="err">{acc.msg.err}</div>}
      {acc.msg?.ok && <div className="ok">{acc.msg.ok}</div>}
      <p className="note tiny">* The marketable part of a limit order that crosses the book pays the taker fee.</p>
    </form>
  );
}

export function AccountSummary({ acc }) {
  const a = acc.data.account;
  const pnl = Number(a.pnl_micros);
  return (
    <div className="acct-sum">
      <div><span>Equity</span><b>{fmt.usd(a.equity_micros)}</b></div>
      <div><span>P&L</span><b className={pnl >= 0 ? "bid" : "ask"}>{fmt.usd(pnl)}</b></div>
      <div><span>Cash</span><b>{fmt.usd(a.cash_micros)}</b></div>
      <div><span>BTC</span><b>{(Number(a.btc) / QTY).toFixed(6)}</b></div>
    </div>
  );
}

export function Positions({ acc, state }) {
  const a = acc.data.account;
  const pd = state?.meta?.price_dec ?? 2;
  const bb = state?.bids?.[0]?.[0],
    ba = state?.asks?.[0]?.[0];
  const mid = bb && ba ? (bb + ba) / 2 / 10 ** pd : null;
  const btc = Number(a.btc) / QTY;
  return (
    <table>
      <thead>
        <tr><th>Asset</th><th>Size</th><th>Mark (mid)</th><th>Value</th><th>Cash</th><th>Equity</th><th>P&L vs $100k</th><th>Fees paid</th></tr>
      </thead>
      <tbody>
        <tr>
          <td>BTC <span className="tag">paper</span></td>
          <td>{btc.toFixed(6)}</td>
          <td>{mid ? usd(mid) : "—"}</td>
          <td>{mid ? usd(btc * mid) : "—"}</td>
          <td>{fmt.usd(a.cash_micros)}</td>
          <td>{fmt.usd(a.equity_micros)}</td>
          <td className={Number(a.pnl_micros) >= 0 ? "bid" : "ask"}>{fmt.usd(a.pnl_micros)}</td>
          <td>{fmt.usd(a.fees_micros)}</td>
        </tr>
      </tbody>
    </table>
  );
}

export function OpenOrders({ acc }) {
  const open = acc.data.account.open;
  if (!open.length) return <p className="empty">No open paper orders.</p>;
  return (
    <table>
      <thead><tr><th>#</th><th>Time</th><th>Side</th><th>Price</th><th>Left</th><th>Queue ahead</th><th></th></tr></thead>
      <tbody>
        {open.map((o) => (
          <tr key={o.id}>
            <td>{o.id}</td>
            <td className="muted">{new Date(o.ts).toLocaleTimeString("en-GB")}</td>
            <td className={o.side === "buy" ? "bid" : "ask"}>{o.side}</td>
            <td>{fmt.price(Number(o.price))}</td>
            <td>{fmt.qty(Number(o.qty))}</td>
            <td>{fmt.qty(Number(o.ahead))}</td>
            <td><button className="mini" onClick={() => acc.act("/api/cancel", { id: o.id }, `cancelled #${o.id}`)}>Cancel</button></td>
          </tr>
        ))}
      </tbody>
    </table>
  );
}

export function OrderHistory({ acc }) {
  const h = acc.data.account.history ?? [];
  if (!h.length) return <p className="empty">No orders yet. Order history starts with this version of the site.</p>;
  return (
    <table>
      <thead><tr><th>#</th><th>Time</th><th>Side</th><th>Type</th><th>Price</th><th>Size</th><th>Filled</th><th>Status</th></tr></thead>
      <tbody>
        {[...h].reverse().map((o) => (
          <tr key={o.id}>
            <td>{o.id}</td>
            <td className="muted">{new Date(o.ts).toLocaleTimeString("en-GB")}</td>
            <td className={o.side === "buy" ? "bid" : "ask"}>{o.side}</td>
            <td>{o.type}</td>
            <td>{o.price === null ? "market" : fmt.price(Number(o.price))}</td>
            <td>{fmt.qty(Number(o.qty))}</td>
            <td>{fmt.qty(Number(o.filled))}</td>
            <td className={"status " + o.status}>{o.status}</td>
          </tr>
        ))}
      </tbody>
    </table>
  );
}

export function Fills({ acc }) {
  const f = acc.data.account.fills;
  if (!f.length) return <p className="empty">No simulated fills yet.</p>;
  return (
    <table>
      <thead><tr><th>Time</th><th>Order</th><th>Side</th><th>Price</th><th>Size</th><th>Liquidity</th><th>Fee</th></tr></thead>
      <tbody>
        {[...f].reverse().map((x, i) => (
          <tr key={i}>
            <td className="muted">{new Date(x.ts).toLocaleTimeString("en-GB")}</td>
            <td>#{x.orderId}</td>
            <td className={x.side === "buy" ? "bid" : "ask"}>{x.side}</td>
            <td>{fmt.price(Number(x.price))}</td>
            <td>{fmt.qty(Number(x.qty))}</td>
            <td>{x.liq === "M" ? "maker" : "taker"}</td>
            <td>{fmt.usd(x.fee)}</td>
          </tr>
        ))}
      </tbody>
    </table>
  );
}

export function AccountSettings({ acc }) {
  const { config, data, act } = acc;
  const ts = useTurnstile(config?.turnstileSiteKey);
  const a = data.account;
  return (
    <div className="settings">
      <p className="note">
        Signed in as <b>{data.user.name}</b>. Limits: {a.limits.max_qty_btc} BTC and $
        {a.limits.max_notional_usd.toLocaleString()} per order, {a.limits.max_open_orders} open orders,{" "}
        {a.limits.max_orders_per_day} orders a day ({a.orders_today} used today). Cash account: no leverage, no shorting.
        Fees 0.30% maker / 0.40% taker (Bitstamp entry tier).
      </p>
      <label className="row note">
        <input type="checkbox" checked={data.hidden} onChange={(e) => act("/api/privacy", { hidden: e.target.checked }, "saved")} />
        Hide me from the leaderboard
      </label>
      <div ref={ts.ref} className="turnstile" />
      <div className="row">
        <button disabled={!!config?.turnstileSiteKey && !ts.token} onClick={() => act("/api/reset", { turnstile: ts.token }, "balance reset to $100,000 (paper)")}>
          Reset paper balance
        </button>
        <button className="danger" disabled={!!config?.turnstileSiteKey && !ts.token} onClick={() => confirm("Delete your paper account and leaderboard entry?") && act("/api/delete", { turnstile: ts.token }, "deleted")}>
          Delete account
        </button>
        <button onClick={() => api("/auth/logout", {}).then(() => location.reload())}>Sign out</button>
      </div>
      <Privacy />
    </div>
  );
}

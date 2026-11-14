// Signed-in paper trading. Every account is its own Durable Object simulator on
// Cloudflare: it reads the public book from the edge and never reaches the engine.
import { useCallback, useEffect, useRef, useState } from "react";
import { fmt } from "./market.js";

function useTurnstile(siteKey) {
  const ref = useRef(null);
  const [token, setToken] = useState("");
  useEffect(() => {
    if (!siteKey || !ref.current) return;
    const render = () => window.turnstile?.render(ref.current, { sitekey: siteKey, callback: setToken, "expired-callback": () => setToken("") });
    if (window.turnstile) render();
    else {
      const s = document.createElement("script");
      s.src = "https://challenges.cloudflare.com/turnstile/v0/api.js";
      s.async = true;
      s.onload = render;
      document.head.appendChild(s);
    }
  }, [siteKey]);
  return { ref, token };
}

async function api(path, body) {
  const r = await fetch(path, body === undefined ? {} : { method: "POST", headers: { "content-type": "application/json" }, body: JSON.stringify(body) });
  const j = await r.json().catch(() => ({ error: `HTTP ${r.status}` }));
  return { status: r.status, ...j };
}

function SignIn({ config }) {
  const { ref, token } = useTurnstile(config?.turnstileSiteKey);
  const providers = config?.providers ?? [];
  return (
    <div className="card" style={{ maxWidth: 560 }}>
      <h2>Sign in to paper trade</h2>
      <p>
        Every signed-in user gets a sandboxed <b>paper</b> account with $100,000 of pretend money, filled against the
        live Bitstamp book with the same queue-position model the C++ simulator uses. Nothing is ever sent to an
        exchange.
      </p>
      {!providers.length && <p className="err">Sign-in isn&apos;t configured on this deployment yet.</p>}
      <div ref={ref} style={{ margin: "10px 0" }} />
      <div className="row">
        {providers.map((p) => (
          <form key={p} method="POST" action={`/auth/${p}/start`}>
            <input type="hidden" name="cf-turnstile-response" value={token} />
            <button className="primary" disabled={!!config?.turnstileSiteKey && !token}>
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
    <details style={{ marginTop: 12 }}>
      <summary className="note">Privacy</summary>
      <p className="note">
        No passwords. Sign-in is through GitHub or Google; the site keeps a hashed account id, your public handle
        (GitHub login, or a Google first name plus 4 characters), and your paper balances and orders. Access tokens are
        used once to read the profile and then discarded; email addresses are never requested or stored. The weekly
        leaderboard shows your handle and paper P&L; you can hide yourself or delete your account (and its leaderboard
        row) below. A session cookie keeps you signed in for 7 days. Cloudflare processes requests as the host.
      </p>
    </details>
  );
}

export function Trade({ state }) {
  const [config, setConfig] = useState(null);
  const [data, setData] = useState(null);
  const [needLogin, setNeedLogin] = useState(false);
  const [msg, setMsg] = useState(null);
  const [form, setForm] = useState({ side: "buy", type: "limit", qty: "0.01", price: "" });
  const resetTs = useTurnstile(config?.turnstileSiteKey);
  const meta = state?.meta;

  const refresh = useCallback(async () => {
    const r = await api("/api/account");
    if (r.status === 401) setNeedLogin(true);
    else if (r.account) {
      setNeedLogin(false);
      setData(r);
    }
  }, []);
  useEffect(() => {
    fetch("/api/config").then((r) => r.json()).then(setConfig).catch(() => setConfig({}));
    refresh();
    const t = setInterval(refresh, 5000);
    return () => clearInterval(t);
  }, [refresh]);

  if (needLogin) return <SignIn config={config} />;
  if (!data) return <p className="note">Loading paper account…</p>;
  const acct = data.account;
  const bestBid = state?.bids?.[0]?.[0],
    bestAsk = state?.asks?.[0]?.[0];

  const submit = async (e) => {
    e.preventDefault();
    const r = await api("/api/order", form.type === "market" ? { side: form.side, type: "market", qty: form.qty } : form);
    setMsg(r.result?.error || r.error ? { err: r.result?.error || r.error } : { ok: `Paper order #${r.result.id}: filled ${fmt.qty(r.result.filled)}${Number(r.result.rested) ? `, resting ${fmt.qty(r.result.rested)}` : ""}` });
    if (r.account) setData(r);
  };
  const act = async (path, body, label) => {
    const r = await api(path, body);
    setMsg(r.error || r.result?.error ? { err: r.error || r.result.error } : { ok: label });
    if (r.account) setData(r);
    if (path === "/api/delete" && r.deleted) location.href = "/";
  };

  return (
    <div className="grid">
      <section className="card span-4">
        <h2>
          Paper account <small>{data.user.name}</small>
        </h2>
        <div className="kpis">
          <div className="kpi"><div className="v">{fmt.usd(acct.equity_micros)}</div><div className="l">equity (paper)</div></div>
          <div className="kpi"><div className={"v " + (Number(acct.pnl_micros) >= 0 ? "bid" : "ask")}>{fmt.usd(acct.pnl_micros)}</div><div className="l">P&L vs $100k</div></div>
          <div className="kpi"><div className="v">{fmt.usd(acct.cash_micros)}</div><div className="l">cash</div></div>
          <div className="kpi"><div className="v">{fmt.qty(Number(acct.btc))}</div><div className="l">BTC</div></div>
        </div>
        <p className="note">
          Fees: 0.30% maker / 0.40% taker (Bitstamp entry tier). Limits: {acct.limits.max_qty_btc} BTC and $
          {acct.limits.max_notional_usd.toLocaleString()} per order, {acct.limits.max_open_orders} open orders,{" "}
          {acct.limits.max_orders_per_day} orders/day ({acct.orders_today} used). No leverage, no shorting.
        </p>
      </section>
      <section className="card span-4">
        <h2>New paper order</h2>
        <form onSubmit={submit} className="row" style={{ flexDirection: "column", alignItems: "stretch" }}>
          <div className="row">
            <button type="button" className={form.side === "buy" ? "buy" : ""} onClick={() => setForm({ ...form, side: "buy" })}>Buy</button>
            <button type="button" className={form.side === "sell" ? "sell" : ""} onClick={() => setForm({ ...form, side: "sell" })}>Sell</button>
            <select value={form.type} onChange={(e) => setForm({ ...form, type: e.target.value })}>
              <option value="limit">Limit</option>
              <option value="market">Market</option>
            </select>
          </div>
          <label className="note">Quantity (BTC)</label>
          <input value={form.qty} onChange={(e) => setForm({ ...form, qty: e.target.value })} inputMode="decimal" />
          {form.type === "limit" && (
            <>
              <label className="note">
                Limit price (USD){" "}
                {bestBid && bestAsk && (
                  <a href="#" onClick={(e) => { e.preventDefault(); setForm({ ...form, price: ((form.side === "buy" ? bestBid : bestAsk) / 100).toFixed(2) }); }}>
                    use touch
                  </a>
                )}
              </label>
              <input value={form.price} onChange={(e) => setForm({ ...form, price: e.target.value })} inputMode="decimal" placeholder={bestBid ? fmt.price(bestBid, meta?.price_dec).replace(/,/g, "") : ""} />
            </>
          )}
          <button className={form.side === "buy" ? "buy" : "sell"}>
            Place PAPER {form.side} order
          </button>
          {msg?.err && <span className="err">{msg.err}</span>}
          {msg?.ok && <span className="ok">{msg.ok}</span>}
        </form>
      </section>
      <section className="card span-4">
        <h2>Open orders</h2>
        {!acct.open.length ? (
          <p className="note">None.</p>
        ) : (
          <table>
            <thead>
              <tr><th>#</th><th>Side</th><th>Price</th><th>Left</th><th>Queue ahead</th><th></th></tr>
            </thead>
            <tbody>
              {acct.open.map((o) => (
                <tr key={o.id}>
                  <td>{o.id}</td>
                  <td className={o.side === "buy" ? "bid" : "ask"}>{o.side}</td>
                  <td>{fmt.price(Number(o.price))}</td>
                  <td>{fmt.qty(Number(o.qty))}</td>
                  <td>{fmt.qty(Number(o.ahead))}</td>
                  <td><button onClick={() => act("/api/cancel", { id: o.id }, `cancelled #${o.id}`)}>cancel</button></td>
                </tr>
              ))}
            </tbody>
          </table>
        )}
      </section>
      <section className="card span-8">
        <h2>Simulated fills</h2>
        {!acct.fills.length ? (
          <p className="note">No fills yet.</p>
        ) : (
          <table>
            <thead><tr><th>Time</th><th>Order</th><th>Side</th><th>Price</th><th>Qty</th><th>Liquidity</th><th>Fee</th></tr></thead>
            <tbody>
              {[...acct.fills].reverse().map((f, i) => (
                <tr key={i}>
                  <td className="muted">{new Date(f.ts).toLocaleTimeString("en-GB")}</td>
                  <td>#{f.orderId}</td>
                  <td className={f.side === "buy" ? "bid" : "ask"}>{f.side}</td>
                  <td>{fmt.price(Number(f.price))}</td>
                  <td>{fmt.qty(Number(f.qty))}</td>
                  <td>{f.liq === "M" ? "maker" : "taker"}</td>
                  <td>{fmt.usd(f.fee)}</td>
                </tr>
              ))}
            </tbody>
          </table>
        )}
      </section>
      <section className="card span-4">
        <h2>Account</h2>
        <label className="row note">
          <input type="checkbox" checked={data.hidden} onChange={(e) => act("/api/privacy", { hidden: e.target.checked }, "saved")} />
          Hide me from the leaderboard
        </label>
        <div ref={resetTs.ref} style={{ margin: "10px 0" }} />
        <div className="row">
          <button disabled={!!config?.turnstileSiteKey && !resetTs.token} onClick={() => act("/api/reset", { turnstile: resetTs.token }, "balance reset to $100,000 (paper)")}>
            Reset paper balance
          </button>
          <button className="danger" disabled={!!config?.turnstileSiteKey && !resetTs.token} onClick={() => confirm("Delete your paper account and leaderboard entry?") && act("/api/delete", { turnstile: resetTs.token }, "deleted")}>
            Delete account
          </button>
          <button onClick={() => api("/auth/logout", {}).then(() => location.reload())}>Sign out</button>
        </div>
        <Privacy />
      </section>
    </div>
  );
}

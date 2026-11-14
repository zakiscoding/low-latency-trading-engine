// Owner-only controls. Served only at /admin after the Worker verifies a
// Cloudflare Access token; the engine also enforces its own risk limits.
import { StrictMode, useState } from "react";
import { createRoot } from "react-dom/client";
import { fmt, useMarket } from "./market.js";
import "./styles.css";

async function command(op, limits) {
  const r = await fetch("/admin/api/command", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify({ op, limits }),
  });
  return r.json().catch(() => ({ ok: false, error: `HTTP ${r.status}` }));
}

function Admin() {
  const { state } = useMarket();
  const [log, setLog] = useState([]);
  const [lim, setLim] = useState({ MaxOrderQty: "", MaxPosition: "", MaxOrdersPerSec: "", MaxOrdersPerDay: "", MaxDailyLoss: "", OrderQty: "" });
  const run = async (op, limits) => {
    const r = await command(op, limits);
    setLog((l) => [`${new Date().toLocaleTimeString()} ${op}: ${r.ok ? "ok" : "FAILED " + (r.error ?? "")} (kill=${r.kill}, trading=${r.trading})`, ...l].slice(0, 30));
  };
  const st = state?.stats ?? {};
  return (
    <>
      <div className="paper-banner">ADMIN · PAPER TRADING ENGINE · NO REAL ORDERS</div>
      <main>
        <div className="grid">
          <section className="card span-6">
            <h2>Strategy</h2>
            <p className="mono">
              status: {st.kill ? "KILL SWITCH ON" : st.trading === false ? "stopped" : "running"} · engine{" "}
              {st.engine_up ? "up" : "down"} · orders {st.orders ?? "—"} · fills {st.fills ?? "—"} · position{" "}
              {st.position !== undefined ? fmt.qty(st.position) : "—"} · ring lost {st.ring_lost ?? 0}
            </p>
            <div className="row">
              <button className="buy" onClick={() => run("start")}>Start strategy</button>
              <button onClick={() => run("stop")}>Stop strategy</button>
              <button className="danger" onClick={() => run("kill")}>KILL SWITCH</button>
              <button onClick={() => run("unkill")}>Release kill switch</button>
              <button onClick={() => run("replay")}>Start ITCH replay</button>
              <button onClick={() => run("status")}>Refresh status</button>
            </div>
          </section>
          <section className="card span-6">
            <h2>Risk limits <small>blank = unchanged; units as the engine uses (qty in 1e-8 BTC)</small></h2>
            {Object.keys(lim).map((k) => (
              <div className="row" key={k} style={{ marginBottom: 6 }}>
                <label className="note" style={{ width: 160 }}>{k}</label>
                <input value={lim[k]} onChange={(e) => setLim({ ...lim, [k]: e.target.value })} inputMode="numeric" />
              </div>
            ))}
            <button
              className="primary"
              onClick={() => run("limits", Object.fromEntries(Object.entries(lim).filter(([, v]) => v !== "").map(([k, v]) => [k, Number(v)])))}
            >
              Apply limits
            </button>
          </section>
          <section className="card span-12">
            <h2>Log</h2>
            <pre className="mono" style={{ margin: 0, whiteSpace: "pre-wrap" }}>{log.join("\n") || "no commands yet"}</pre>
          </section>
        </div>
      </main>
    </>
  );
}

createRoot(document.getElementById("root")).render(
  <StrictMode>
    <Admin />
  </StrictMode>,
);

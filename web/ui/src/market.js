// Live market stream: snapshot then deltas over one WebSocket, with reconnect.
import { useEffect, useRef, useState } from "react";

function applyLevels(levels, changes, desc) {
  const m = new Map(levels);
  for (const [p, q] of changes ?? []) {
    if (q === 0) m.delete(p);
    else m.set(p, q);
  }
  return [...m.entries()].sort((a, b) => (desc ? b[0] - a[0] : a[0] - b[0])).slice(0, 20);
}

function mergeBuckets(buckets, add) {
  const m = new Map(buckets ?? []);
  for (const [ns, c] of add ?? []) m.set(ns, (m.get(ns) ?? 0) + c);
  return [...m.entries()].sort((a, b) => a[0] - b[0]);
}

export function reduce(state, msg) {
  if (msg.t === "snap")
    return { ...msg, bids: msg.bids ?? [], asks: msg.asks ?? [], trades: msg.trades ?? [], fills: msg.fills ?? [], connected: true };
  if (msg.t === "offline") return { ...(state ?? {}), offline: true, connected: true };
  if (msg.t === "upstream") return state ? { ...state, upstream: msg.up } : state;
  if (msg.t !== "d" || !state || !state.meta) return state;
  const s = { ...state, upstream: true, seq: msg.seq, ts: msg.ts };
  s.bids = applyLevels(state.bids ?? [], msg.bids, true);
  s.asks = applyLevels(state.asks ?? [], msg.asks, false);
  if (msg.trades?.length) s.trades = [...(state.trades ?? []), ...msg.trades].slice(-60);
  if (msg.fills?.length) s.fills = [...(state.fills ?? []), ...msg.fills].slice(-40);
  if (msg.lat_add?.length) {
    const t = { ...(state.latency?.tick_to_trade ?? {}) };
    t.buckets = mergeBuckets(t.buckets, msg.lat_add);
    if (msg.lat) Object.assign(t, msg.lat);
    s.latency = { ...state.latency, tick_to_trade: t };
  }
  if (msg.stats) s.stats = msg.stats;
  return s;
}

export function useMarket() {
  const [state, setState] = useState(null);
  const [conn, setConn] = useState("connecting");
  const retry = useRef(1000);
  useEffect(() => {
    let ws,
      closed = false,
      timer;
    const connect = () => {
      const proto = location.protocol === "https:" ? "wss" : "ws";
      ws = new WebSocket(`${proto}://${location.host}/ws`);
      ws.onopen = () => {
        setConn("live");
        retry.current = 1000;
      };
      ws.onmessage = (e) => {
        try {
          const m = JSON.parse(e.data);
          setState((s) => reduce(s, m));
        } catch {}
      };
      ws.onclose = () => {
        if (closed) return;
        setConn("reconnecting");
        timer = setTimeout(connect, retry.current);
        retry.current = Math.min(retry.current * 2, 30000);
      };
    };
    // Show the edge-cached snapshot immediately, then go live.
    fetch("/api/snapshot")
      .then((r) => r.json())
      .then((m) => setState((s) => s ?? reduce(null, m)))
      .catch(() => {});
    connect();
    return () => {
      closed = true;
      clearTimeout(timer);
      ws?.close();
    };
  }, []);
  return { state, conn };
}

export const fmt = {
  price: (cents, dec = 2) => (cents / 10 ** dec).toLocaleString("en-US", { minimumFractionDigits: dec, maximumFractionDigits: dec }),
  qty: (units, dec = 8) => (units / 10 ** dec).toFixed(Math.min(dec, 6)),
  usd: (micros) => {
    const v = Number(micros) / 1e6;
    return (v < 0 ? "-$" : "$") + Math.abs(v).toLocaleString("en-US", { minimumFractionDigits: 2, maximumFractionDigits: 2 });
  },
  ns: (ns) => (ns >= 1e6 ? (ns / 1e6).toFixed(2) + " ms" : ns >= 1e3 ? (ns / 1e3).toFixed(1) + " µs" : Math.round(ns) + " ns"),
  time: (ns) => new Date(ns / 1e6).toLocaleTimeString("en-GB", { hour12: false }),
};

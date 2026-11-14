// Candlestick chart on a canvas: candles + volume, price/time axes, crosshair with
// an OHLC readout, last-price line, and markers for the viewer's paper fills.
// Wheel zooms, drag pans, double-click goes back to the latest bar.
import { useEffect, useRef, useState } from "react";

const css = (n) => getComputedStyle(document.documentElement).getPropertyValue(n).trim();
const AXIS_W = 72;
const TIME_H = 22;

function niceStep(range, target) {
  const raw = range / Math.max(1, target);
  const p = 10 ** Math.floor(Math.log10(raw));
  for (const m of [1, 2, 2.5, 5, 10]) if (m * p >= raw) return m * p;
  return 10 * p;
}

function timeLabel(ms, tfMin, first) {
  const d = new Date(ms);
  const hh = String(d.getHours()).padStart(2, "0"),
    mm = String(d.getMinutes()).padStart(2, "0");
  if (first || (d.getHours() === 0 && d.getMinutes() === 0))
    return d.toLocaleDateString("en-GB", { day: "numeric", month: "short" }) + (tfMin < 60 ? ` ${hh}:${mm}` : "");
  return `${hh}:${mm}`;
}

export function CandleChart({ bars, tfMin, priceDec = 2, qtyDec = 8, markers = [], symbol = "" }) {
  const wrap = useRef(null);
  const cv = useRef(null);
  const view = useRef({ barW: 8, offset: 0 }); // offset: bars scrolled back from the newest
  const mouse = useRef(null);
  const drag = useRef(null);
  const [, force] = useState(0);
  const redraw = () => force((x) => x + 1);

  // Reset scroll when the interval changes.
  useEffect(() => {
    view.current.offset = 0;
    redraw();
  }, [tfMin]);

  useEffect(() => {
    const c = cv.current,
      el = wrap.current;
    if (!c || !el) return;
    const dpr = window.devicePixelRatio || 1;
    const W = el.clientWidth,
      H = el.clientHeight;
    if (W < 10 || H < 10) return;
    c.width = Math.round(W * dpr);
    c.height = Math.round(H * dpr);
    c.style.width = W + "px";
    c.style.height = H + "px";
    const g = c.getContext("2d");
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.clearRect(0, 0, W, H);
    const C = {
      up: css("--bid"),
      down: css("--ask"),
      grid: css("--grid"),
      text: css("--muted"),
      fg: css("--text"),
      cross: css("--cross"),
      label: css("--panel-3"),
      mono: css("--mono"),
    };
    g.font = `11px ${C.mono}`;
    const plotW = W - AXIS_W,
      plotH = H - TIME_H;
    if (!bars.length) {
      g.fillStyle = C.text;
      g.textAlign = "center";
      g.fillText("Collecting trades: candles are built from the live trades the engine streams", plotW / 2, plotH / 2);
      return;
    }
    const v = view.current;
    const n = Math.max(5, Math.floor(plotW / v.barW));
    v.offset = Math.max(0, Math.min(v.offset, bars.length - 5));
    const end = bars.length - Math.round(v.offset);
    const start = Math.max(0, end - n);
    const vis = bars.slice(start, end);
    const volH = plotH * 0.18;
    const priceH = plotH - volH - 8;
    let hi = -Infinity,
      lo = Infinity,
      vmax = 0;
    for (const b of vis) {
      if (b[2] > hi) hi = b[2];
      if (b[3] < lo) lo = b[3];
      if (b[5] > vmax) vmax = b[5];
    }
    if (hi === lo) {
      hi += 1;
      lo -= 1;
    }
    const padP = (hi - lo) * 0.08;
    hi += padP;
    lo -= padP;
    const scale = 10 ** priceDec;
    const TOP = 30; // room for the OHLC readout
    const yP = (p) => TOP + ((hi - p) / (hi - lo)) * (priceH - TOP);
    const pOfY = (y) => hi - ((y - TOP) / (priceH - TOP)) * (hi - lo);
    // Bars are right-aligned: the newest visible bar sits next to the price axis.
    const xOf = (i) => plotW - (vis.length - i - 0.5) * v.barW - 4;
    const fmtP = (p) => (p / scale).toLocaleString("en-US", { minimumFractionDigits: priceDec, maximumFractionDigits: priceDec });

    // Grid + price axis.
    const step = niceStep((hi - lo) / scale, priceH / 48) * scale;
    g.textAlign = "left";
    g.textBaseline = "middle";
    for (let p = Math.ceil(lo / step) * step; p <= hi; p += step) {
      const y = Math.round(yP(p)) + 0.5;
      g.fillStyle = C.grid;
      g.fillRect(0, y, plotW, 1);
      g.fillStyle = C.text;
      g.fillText(fmtP(p), plotW + 6, y);
    }
    // Time axis: a label roughly every 90 px.
    const every = Math.max(1, Math.round(90 / v.barW));
    g.textAlign = "center";
    g.textBaseline = "alphabetic";
    let firstLabel = true;
    for (let i = 0; i < vis.length; i++) {
      const gi = start + i;
      if (gi % every) continue;
      const x = Math.round(xOf(i)) + 0.5;
      g.fillStyle = C.grid;
      g.fillRect(x, 0, 1, plotH);
      g.fillStyle = C.text;
      const lbl = timeLabel(vis[i][0], tfMin, firstLabel);
      const half = g.measureText(lbl).width / 2 + 2;
      g.fillText(lbl, Math.max(half, Math.min(plotW - half, x)), H - 7);
      firstLabel = false;
    }
    g.fillStyle = C.grid;
    g.fillRect(plotW, 0, 1, H);
    g.fillRect(0, plotH, W, 1);

    // Volume, then candles.
    const bw = Math.max(1, Math.floor(v.barW * 0.7));
    vis.forEach((b, i) => {
      const x = xOf(i);
      const up = b[4] >= b[1];
      const vh = vmax ? (b[5] / vmax) * volH : 0;
      g.globalAlpha = 0.35;
      g.fillStyle = up ? C.up : C.down;
      g.fillRect(Math.round(x - bw / 2), plotH - vh, bw, vh);
      g.globalAlpha = 1;
      const yo = yP(b[1]),
        yc = yP(b[4]);
      g.fillRect(Math.round(x) , yP(b[2]), 1, Math.max(1, yP(b[3]) - yP(b[2])));
      g.fillRect(Math.round(x - bw / 2), Math.min(yo, yc), bw, Math.max(1, Math.abs(yc - yo)));
    });

    // Paper fill markers: buys below the bar, sells above.
    const t0 = vis[0][0],
      tfMs = tfMin * 60_000;
    for (const m of markers) {
      if (m.t < t0 || m.t >= vis[vis.length - 1][0] + tfMs) continue;
      let i = vis.findIndex((b) => b[0] + tfMs > m.t);
      if (i < 0) continue;
      const x = xOf(i);
      const buy = m.side === "buy";
      const y = buy ? yP(vis[i][3]) + 10 : yP(vis[i][2]) - 10;
      g.fillStyle = buy ? C.up : C.down;
      g.beginPath();
      if (buy) {
        g.moveTo(x, y - 5);
        g.lineTo(x - 5, y + 4);
        g.lineTo(x + 5, y + 4);
      } else {
        g.moveTo(x, y + 5);
        g.lineTo(x - 5, y - 4);
        g.lineTo(x + 5, y - 4);
      }
      g.closePath();
      g.fill();
    }

    // Last price line + label.
    const last = bars[bars.length - 1];
    const lastUp = last[4] >= last[1];
    const yl = Math.round(yP(last[4])) + 0.5;
    if (yl > 0 && yl < priceH + 8) {
      g.strokeStyle = lastUp ? C.up : C.down;
      g.setLineDash([2, 3]);
      g.beginPath();
      g.moveTo(0, yl);
      g.lineTo(plotW, yl);
      g.stroke();
      g.setLineDash([]);
      g.fillStyle = lastUp ? C.up : C.down;
      g.fillRect(plotW + 1, yl - 9, AXIS_W - 1, 18);
      g.fillStyle = "#0b0e13";
      g.textAlign = "left";
      g.textBaseline = "middle";
      g.fillText(fmtP(last[4]), plotW + 6, yl);
    }

    // Crosshair.
    const mpos = mouse.current;
    let hover = null;
    if (mpos && mpos.x < plotW && mpos.y < plotH) {
      const i = Math.max(0, Math.min(vis.length - 1, Math.round(vis.length - 0.5 - (plotW - 4 - mpos.x) / v.barW)));
      hover = vis[i];
      const x = Math.round(xOf(i)) + 0.5;
      g.strokeStyle = C.cross;
      g.setLineDash([4, 4]);
      g.beginPath();
      g.moveTo(x, 0);
      g.lineTo(x, plotH);
      g.moveTo(0, Math.round(mpos.y) + 0.5);
      g.lineTo(plotW, Math.round(mpos.y) + 0.5);
      g.stroke();
      g.setLineDash([]);
      g.fillStyle = C.label;
      if (mpos.y < priceH + 8) {
        g.fillRect(plotW + 1, mpos.y - 9, AXIS_W - 1, 18);
        g.fillStyle = C.fg;
        g.textAlign = "left";
        g.textBaseline = "middle";
        g.fillText(fmtP(pOfY(mpos.y)), plotW + 6, mpos.y);
      }
      const tl = new Date(hover[0]).toLocaleString("en-GB", { day: "numeric", month: "short", hour: "2-digit", minute: "2-digit" });
      const tw = g.measureText(tl).width + 12;
      g.fillStyle = C.label;
      g.fillRect(x - tw / 2, plotH + 1, tw, TIME_H - 1);
      g.fillStyle = C.fg;
      g.textAlign = "center";
      g.textBaseline = "middle";
      g.fillText(tl, x, plotH + TIME_H / 2);
    }
    // OHLC readout for the bar under the cursor (or the newest bar).
    const b = hover ?? last;
    const chg = b[1] ? ((b[4] - b[1]) / b[1]) * 100 : 0;
    const col = b[4] >= b[1] ? C.up : C.down;
    const vol = (b[5] / 10 ** qtyDec).toLocaleString("en-US", { maximumFractionDigits: 4 });
    const wide = plotW > 520; // phones: OHLC only
    const parts = [
      [wide && symbol ? `${symbol} · ${tfMin >= 60 ? tfMin / 60 + "h" : tfMin + "m"}  ` : "", C.fg],
      ["O ", C.text], [fmtP(b[1]) + "  ", col],
      ["H ", C.text], [fmtP(b[2]) + "  ", col],
      ["L ", C.text], [fmtP(b[3]) + "  ", col],
      ["C ", C.text], [fmtP(b[4]) + "  ", col],
      [wide ? (chg >= 0 ? "+" : "") + chg.toFixed(2) + "%  " : "", col],
      [wide ? "Vol " : "", C.text], [wide ? vol : "", C.fg],
    ];
    g.textAlign = "left";
    g.textBaseline = "top";
    let tx = 8;
    for (const [t, c] of parts) {
      if (!t) continue;
      g.fillStyle = c;
      g.fillText(t, tx, 8);
      tx += g.measureText(t).width;
    }
  });

  useEffect(() => {
    const el = wrap.current;
    if (!el) return;
    const ro = new ResizeObserver(() => redraw());
    ro.observe(el);
    const wheel = (e) => {
      e.preventDefault();
      const v = view.current;
      v.barW = Math.max(2, Math.min(40, v.barW * (e.deltaY > 0 ? 0.88 : 1.14)));
      redraw();
    };
    el.addEventListener("wheel", wheel, { passive: false });
    return () => {
      ro.disconnect();
      el.removeEventListener("wheel", wheel);
    };
  }, []);

  const pos = (e) => {
    const r = cv.current.getBoundingClientRect();
    return { x: e.clientX - r.left, y: e.clientY - r.top };
  };
  return (
    <div
      ref={wrap}
      className="chart-wrap"
      onPointerMove={(e) => {
        mouse.current = pos(e);
        if (drag.current) {
          const dx = e.clientX - drag.current.x;
          view.current.offset = Math.max(0, drag.current.offset + dx / view.current.barW);
        }
        redraw();
      }}
      onPointerLeave={() => {
        mouse.current = null;
        drag.current = null;
        redraw();
      }}
      onPointerDown={(e) => {
        drag.current = { x: e.clientX, offset: view.current.offset };
        e.currentTarget.setPointerCapture?.(e.pointerId);
      }}
      onPointerUp={() => (drag.current = null)}
      onDoubleClick={() => {
        view.current.offset = 0;
        redraw();
      }}
    >
      <canvas ref={cv} />
    </div>
  );
}

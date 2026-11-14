// Canvas charts: order-book depth and log-scale latency histograms.
import { useEffect, useRef } from "react";
import { fmt } from "./market.js";

function useCanvas(draw, deps, height) {
  const ref = useRef(null);
  useEffect(() => {
    const c = ref.current;
    if (!c) return;
    const paint = () => {
      const dpr = window.devicePixelRatio || 1;
      const w = c.clientWidth;
      c.width = Math.round(w * dpr);
      c.height = Math.round(height * dpr);
      const g = c.getContext("2d");
      g.setTransform(dpr, 0, 0, dpr, 0, 0);
      g.clearRect(0, 0, w, height);
      draw(g, w, height);
    };
    paint();
    const ro = new ResizeObserver(paint);
    ro.observe(c);
    return () => ro.disconnect();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, deps);
  return <canvas ref={ref} style={{ height }} />;
}

const css = (name) => getComputedStyle(document.documentElement).getPropertyValue(name).trim();

export function DepthChart({ bids, asks, priceDec = 2, qtyDec = 8, height = 220 }) {
  return useCanvas(
    (g, w, h) => {
      bids = bids ?? [];
      asks = asks ?? [];
      if (!bids.length || !asks.length) {
        g.fillStyle = css("--muted");
        g.fillText("waiting for book…", 10, 20);
        return;
      }
      const cum = (lv) => {
        let s = 0;
        return lv.map(([p, q]) => [p, (s += q)]);
      };
      const cb = cum(bids),
        ca = cum(asks);
      const lo = cb[cb.length - 1][0],
        hi = ca[ca.length - 1][0];
      const maxQ = Math.max(cb[cb.length - 1][1], ca[ca.length - 1][1]);
      const pad = 24;
      const x = (p) => pad + ((p - lo) / (hi - lo || 1)) * (w - 2 * pad);
      const y = (q) => h - pad - (q / maxQ) * (h - 2 * pad);
      const side = (pts, color, soft) => {
        g.beginPath();
        g.moveTo(x(pts[0][0]), h - pad);
        for (let i = 0; i < pts.length; i++) {
          g.lineTo(x(pts[i][0]), y(i ? pts[i - 1][1] : 0));
          g.lineTo(x(pts[i][0]), y(pts[i][1]));
        }
        g.lineTo(x(pts[pts.length - 1][0]), h - pad);
        g.closePath();
        g.fillStyle = soft;
        g.fill();
        g.strokeStyle = color;
        g.lineWidth = 1.5;
        g.stroke();
      };
      side(cb, css("--bid"), css("--bid-soft"));
      side(ca, css("--ask"), css("--ask-soft"));
      g.fillStyle = css("--muted");
      g.font = "11px " + css("--mono");
      g.fillText(fmt.price(lo, priceDec), pad, h - 6);
      const hs = fmt.price(hi, priceDec);
      g.fillText(hs, w - pad - g.measureText(hs).width, h - 6);
      g.fillText(fmt.qty(maxQ, qtyDec), pad, pad - 8);
    },
    [bids, asks, priceDec, qtyDec],
    height,
  );
}

// buckets: [[upper_ns, count], ...] (log-linear from the engine's histogram).
// Re-binned into log10 bins for display; markers at p50/p99/p99.9.
export function LatencyHist({ buckets, marks = {}, height = 200, color }) {
  return useCanvas(
    (g, w, h) => {
      buckets = buckets ?? [];
      g.font = "11px " + css("--mono");
      if (!buckets.length) {
        g.fillStyle = css("--muted");
        g.fillText("no samples yet", 10, 20);
        return;
      }
      const lo = Math.max(1, Math.floor(Math.log10(buckets[0][0]) * 4) / 4);
      const hi = Math.ceil(Math.log10(buckets[buckets.length - 1][0] + 1) * 4) / 4 + 0.25;
      const nb = Math.max(8, Math.round((hi - lo) * 12));
      const bins = new Array(nb).fill(0);
      for (const [ns, c] of buckets) {
        const i = Math.min(nb - 1, Math.max(0, Math.floor(((Math.log10(ns + 1) - lo) / (hi - lo)) * nb)));
        bins[i] += c;
      }
      const maxC = Math.max(...bins);
      const pad = 26,
        bw = (w - 2 * pad) / nb;
      g.fillStyle = color || css("--accent");
      bins.forEach((c, i) => {
        if (!c) return;
        // sqrt scale so the tail stays visible
        const bh = Math.sqrt(c / maxC) * (h - 2 * pad);
        g.fillRect(pad + i * bw + 0.5, h - pad - bh, Math.max(1, bw - 1), bh);
      });
      const xOf = (ns) => pad + ((Math.log10(ns) - lo) / (hi - lo)) * (w - 2 * pad);
      g.fillStyle = css("--muted");
      for (let d = Math.ceil(lo); d <= Math.floor(hi); d++) {
        const xx = xOf(10 ** d);
        g.fillRect(xx, h - pad, 1, 4);
        g.fillText(fmt.ns(10 ** d), xx - 12, h - 8);
      }
      const labels = [
        ["p50", marks.p50],
        ["p99", marks.p99],
        ["p99.9", marks.p999],
      ];
      labels.forEach(([name, v], k) => {
        if (!v) return;
        const xx = xOf(v);
        g.strokeStyle = css("--warn");
        g.setLineDash([3, 3]);
        g.beginPath();
        g.moveTo(xx, pad - 4);
        g.lineTo(xx, h - pad);
        g.stroke();
        g.setLineDash([]);
        g.fillStyle = css("--warn");
        g.fillText(`${name} ${fmt.ns(v)}`, Math.min(xx + 3, w - 90), pad + 10 + k * 13);
      });
    },
    [buckets, marks.p50, marks.p99, marks.p999, color],
    height,
  );
}

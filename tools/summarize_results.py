#!/usr/bin/env python3
"""Builds results/<machine>/optimization_ladder.json and a markdown table from the raw
benchmark JSON (medians across repeats). The README and the website both read these
files, so the published numbers can't drift from the measurements.

usage: python3 tools/summarize_results.py results/laptop
"""
import json
import statistics
import sys
from pathlib import Path

STEPS = [
    ("v0_naive", "Baseline: std::map book, allocate+decode each message, text order build"),
    ("v1_flat_book", "Flat price-level arrays + preallocated order pool (Fibonacci-hashed order map)"),
    ("v2_inplace_parse", "Zero-copy in-place ITCH parsing"),
    ("v3_preserialized", "Pre-serialized order templates (patch 4 fields, send)"),
    ("v4_identity_hash", "Identity hash for order refs: live orders stay in a cache-friendly window"),
    ("v5_hugepages", "Transparent huge pages + prefaulted tables"),
]
COMPILER = [
    ("o2", "-O2, generic x86-64-v2, no LTO"),
    ("o3n", "-O3 -march=native"),
    ("lto", "+ link-time optimization"),
    ("pgo", "+ PGO (trained on synthetic data, measured on the real file)"),
]


def med(xs):
    return int(statistics.median(xs)) if xs else 0


def summarize_variants(variants, name):
    runs = [v for v in variants if v["name"] == name]
    if not runs:
        return None
    t = [r["stages"]["tick_to_trade"] for r in runs]
    b = [r["stages"]["book"] for r in runs]
    return {
        "runs": len(runs),
        "p50": med([x["p50"] for x in t]),
        "p99": med([x["p99"] for x in t]),
        "p999": med([x["p999"] for x in t]),
        "book_p50": med([x["p50"] for x in b]),
        "book_p99": med([x["p99"] for x in b]),
        "msgs_per_sec": med([r["msgs_per_sec"] for r in runs]),
    }


def main(d):
    d = Path(d)
    out = {"bench": "optimization_ladder", "machine": d.name, "rows": []}
    pipe = d / "pipeline_itch_real.json"
    if pipe.exists():
        p = json.loads(pipe.read_text())
        out["title"] = f"In-process tick-to-trade on {p['messages']:,} real NASDAQ ITCH messages (8 most active symbols)"
        out["cpu"] = p["cpu"]
        for name, change in STEPS:
            s = summarize_variants(p["variants"], name)
            if s:
                out["rows"].append({"step": name, "change": change, **s, "throughput": f"{s['msgs_per_sec']/1e6:.2f}M msg/s"})
    for key, change in COMPILER:
        f = d / f"compiler_{key}.json"
        if not f.exists():
            continue
        c = json.loads(f.read_text())
        s = summarize_variants(c["variants"], "v5_hugepages")
        out["rows"].append({"step": "compiler:" + key, "change": change, **s, "throughput": f"{s['msgs_per_sec']/1e6:.2f}M msg/s"})
    (d / "optimization_ladder.json").write_text(json.dumps(out, indent=1) + "\n", encoding="utf-8", newline="\n")

    def ns(v):
        if v >= 1_000_000:
            return f"{v/1e6:.2f} ms"
        return f"{v/1000:.2f} µs" if v >= 1000 else f"{v} ns"

    lines = [
        "| Step | What changed | tick-to-trade p50 | p99 | p99.9 | book stage p50 | throughput |",
        "|---|---|---:|---:|---:|---:|---:|",
    ]
    for r in out["rows"]:
        lines.append(f"| {r['step']} | {r['change']} | {ns(r['p50'])} | {ns(r['p99'])} | {ns(r['p999'])} | {ns(r['book_p50'])} | {r['throughput']} |")
    udp = sorted(d.glob("udp_*.json"))
    if udp:
        lines += ["", "| UDP receive mode | orders | kernel RX → send() p50 | p99 | p99.9 | in-process p50 |", "|---|---:|---:|---:|---:|---:|"]
        for f in udp:
            if f.name.endswith(".exchange.json"):
                continue
            u = json.loads(f.read_text())
            k = u["kernel_rx_to_send"]
            lines.append(f"| {u['rx']} | {u['orders']} | {ns(k['p50'])} | {ns(k['p99'])} | {ns(k['p999'])} | {ns(u['stages']['tick_to_trade']['p50'])} |")
    (d / "TABLE.md").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print("\n".join(lines))


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "results/laptop")

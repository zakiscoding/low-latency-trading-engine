#!/usr/bin/env python3
"""Fails if any tracked file contains a live (real-money) trading endpoint.

This project is paper trading only. The only order-entry host allowed anywhere is
Alpaca's paper API. Patterns are written as escaped regexes so this file doesn't
match itself.
"""
import re
import subprocess
import sys

LIVE = [
    (r"(?<![\w-])api\.alpaca\.markets", "Alpaca live trading API"),
    (r"broker-api\.alpaca\.markets", "Alpaca Broker API"),
    (r"api\.coinbase\.com/api/v3/brokerage", "Coinbase Advanced Trade order API"),
    (r"api\.exchange\.coinbase\.com", "Coinbase Exchange REST (order entry)"),
    (r"api\.pro\.coinbase\.com", "Coinbase Pro REST"),
    (r"api\.prime\.coinbase\.com", "Coinbase Prime"),
    (r"api\.binance\.(com|us)/(api|sapi)/v\d+/order", "Binance order API"),
    (r"api\.kraken\.com/0/private", "Kraken private API"),
    (r"api\.tradier\.com", "Tradier live API"),
    (r"api\.tdameritrade\.com|api\.schwabapi\.com", "Schwab/TDA API"),
    (r"interactivebrokers\.com/v1/api/iserver/account/.*/orders", "IBKR order API"),
]
SKIP_SUFFIX = (".png", ".jpg", ".ico", ".woff", ".woff2", ".gz")


def scan(files):
    bad = []
    for f in files:
        if f.endswith(SKIP_SUFFIX):
            continue
        try:
            text = open(f, encoding="utf-8", errors="ignore").read()
        except OSError:
            continue
        for pat, what in LIVE:
            for m in re.finditer(pat, text):
                line = text.count("\n", 0, m.start()) + 1
                bad.append(f"{f}:{line}: {what}: {m.group(0)}")
    return bad


def self_test():
    # Build sample strings at runtime so they never appear literally in the repo.
    live = "https://" + "api." + "alpaca.markets/v2/orders"
    paper = "https://paper-" + "api." + "alpaca.markets/v2/orders"
    assert any(re.search(p, live) for p, _ in LIVE), "live Alpaca URL not caught"
    assert not any(re.search(p, paper) for p, _ in LIVE), "paper URL wrongly flagged"
    cb = "https://api." + "coinbase.com/api/v3/brokerage/orders"
    assert any(re.search(p, cb) for p, _ in LIVE), "Coinbase order URL not caught"
    assert not any(re.search(p, "wss://advanced-trade-ws.coinbase.com") for p, _ in LIVE)


if __name__ == "__main__":
    self_test()
    files = subprocess.run(["git", "ls-files"], capture_output=True, text=True, check=True).stdout.split()
    bad = scan(files)
    if bad:
        print("Live trading endpoints found (this repo is paper-only):")
        print("\n".join(bad))
        sys.exit(1)
    print(f"ok: {len(files)} tracked files, no live trading endpoints")

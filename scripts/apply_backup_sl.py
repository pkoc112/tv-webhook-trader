#!/usr/bin/env python3
"""
apply_backup_sl.py -- 기존 오픈 포지션에 백업 SL 일괄 적용
거래소에서 열린 포지션 중 SL이 없는 것에 3% 백업 SL 설정

Usage:
    python3 scripts/apply_backup_sl.py                 # dry-run (확인만)
    python3 scripts/apply_backup_sl.py --execute       # 실제 적용
    python3 scripts/apply_backup_sl.py --pct 0.05      # 5% SL 적용
"""

import json
import hmac
import hashlib
import base64
import time
import sys
import os
from urllib.request import Request, urlopen
from urllib.error import HTTPError
from datetime import datetime, timezone

# ─── Config ───
API_KEYS_PATH = os.environ.get(
    "API_KEYS_PATH",
    os.path.join(os.path.dirname(__file__), "..", "config", "api_keys.json")
)
BASE_URL = "https://api.bitget.com"
PRODUCT_TYPE = "USDT-FUTURES"
BACKUP_SL_PCT = 0.03  # 3%

# ─── Bitget Auth ───
def load_keys():
    with open(API_KEYS_PATH) as f:
        return json.load(f)

def sign_request(secret, timestamp, method, path, body=""):
    message = f"{timestamp}{method}{path}{body}"
    mac = hmac.new(secret.encode(), message.encode(), hashlib.sha256)
    return base64.b64encode(mac.digest()).decode()

def api_request(method, path, body=None, keys=None):
    timestamp = str(int(time.time() * 1000))
    body_str = json.dumps(body) if body else ""
    signature = sign_request(keys["api_secret"], timestamp, method, path, body_str)

    headers = {
        "ACCESS-KEY": keys["api_key"],
        "ACCESS-SIGN": signature,
        "ACCESS-TIMESTAMP": timestamp,
        "ACCESS-PASSPHRASE": keys["passphrase"],
        "Content-Type": "application/json",
        "locale": "en-US"
    }

    url = BASE_URL + path
    req = Request(url, method=method, headers=headers)
    if body_str and method == "POST":
        req.data = body_str.encode()

    try:
        resp = urlopen(req, timeout=15)
        return json.loads(resp.read().decode())
    except HTTPError as e:
        err_body = e.read().decode() if e.fp else str(e)
        print(f"  HTTP {e.code}: {err_body}")
        return {"code": str(e.code), "msg": err_body}

# ─── Contract Info ───
def fetch_contracts(keys):
    """심볼별 계약 정보 조회 (pricePlace, sizeMultiplier 등)"""
    path = f"/api/v2/mix/market/contracts?productType={PRODUCT_TYPE}"
    resp = api_request("GET", path, keys=keys)
    if resp.get("code") != "00000":
        print(f"Error fetching contracts: {resp}")
        return {}
    contracts = {}
    for c in resp.get("data", []):
        sym = c.get("symbol", "")
        if sym:
            contracts[sym] = {
                "pricePlace": int(c.get("pricePlace", "4")),
                "priceEndStep": float(c.get("priceEndStep", "1")),
                "sizeMultiplier": float(c.get("sizeMultiplier", "0.001")),
                "minTradeNum": float(c.get("minTradeNum", "0.001")),
            }
    return contracts

def round_price_down(price, price_place, price_end_step=1):
    """거래소 가격 정밀도에 맞게 내림 (long SL용)"""
    import math
    factor = 10 ** price_place
    rounded = math.floor(price * factor) / factor
    if price_end_step > 1:
        last_digit = int(round(rounded * factor)) % int(price_end_step)
        if last_digit != 0:
            rounded = math.floor(rounded * factor / price_end_step) * price_end_step / factor
    return rounded

def round_price_up(price, price_place, price_end_step=1):
    """거래소 가격 정밀도에 맞게 올림 (short SL용)"""
    import math
    factor = 10 ** price_place
    rounded = math.ceil(price * factor) / factor
    if price_end_step > 1:
        last_digit = int(round(rounded * factor)) % int(price_end_step)
        if last_digit != 0:
            rounded = math.ceil(rounded * factor / price_end_step) * price_end_step / factor
    return rounded

# ─── Main Logic ───
def get_all_positions(keys):
    """거래소에서 열린 포지션 전부 조회"""
    path = f"/api/v2/mix/position/all-position?productType={PRODUCT_TYPE}"
    resp = api_request("GET", path, keys=keys)
    if resp.get("code") != "00000":
        print(f"Error fetching positions: {resp}")
        return []
    return resp.get("data", [])

def get_pending_tpsl(keys, symbol=None):
    """대기 중인 TP/SL 트리거 주문 조회"""
    path = f"/api/v2/mix/order/orders-plan-pending?productType={PRODUCT_TYPE}"
    if symbol:
        path += f"&symbol={symbol}"
    resp = api_request("GET", path, keys=keys)
    if resp.get("code") != "00000":
        return []
    data = resp.get("data", {})
    if isinstance(data, dict):
        return data.get("entrustedList", [])
    return data if isinstance(data, list) else []

def place_tpsl_order(keys, symbol, trigger_price, hold_side, size, contracts, dry_run=True):
    """SL 트리거 주문 설정"""
    # 심볼별 가격 정밀도 적용
    ci = contracts.get(symbol, {})
    pp = ci.get("pricePlace", 4)
    pes = ci.get("priceEndStep", 1)
    # long SL은 내림, short SL은 올림 (보수적)
    if hold_side == "long":
        trigger_price = round_price_down(trigger_price, pp, pes)
    else:
        trigger_price = round_price_up(trigger_price, pp, pes)
    trigger_str = f"{trigger_price:.{pp}f}"

    body = {
        "symbol": symbol,
        "productType": PRODUCT_TYPE,
        "marginMode": "crossed",
        "marginCoin": "USDT",
        "planType": "loss_plan",
        "triggerPrice": trigger_str,
        "triggerType": "mark_price",
        "size": str(size),
        "holdSide": hold_side
    }

    if dry_run:
        print(f"  [DRY-RUN] Would place SL: {symbol} @ {trigger_str} hold={hold_side} size={size}")
        return True

    path = "/api/v2/mix/order/place-tpsl-order"
    resp = api_request("POST", path, body=body, keys=keys)
    code = resp.get("code", "99999")
    if code == "00000":
        print(f"  [OK] SL set: {symbol} @ {trigger_str} hold={hold_side}")
        return True
    else:
        print(f"  [FAIL] {symbol}: code={code} msg={resp.get('msg', 'unknown')}")
        return False

def main():
    # Parse args
    dry_run = "--execute" not in sys.argv
    sl_pct = BACKUP_SL_PCT
    for i, arg in enumerate(sys.argv):
        if arg == "--pct" and i + 1 < len(sys.argv):
            sl_pct = float(sys.argv[i + 1])

    mode_str = "DRY-RUN (--execute 로 실행)" if dry_run else "LIVE EXECUTION"
    print(f"{'='*60}")
    print(f"  Backup SL Applicator | {mode_str}")
    print(f"  SL: {sl_pct*100:.1f}% from entry price")
    print(f"{'='*60}")

    keys = load_keys()
    print(f"\nAPI Key: {keys['api_key'][:12]}...")

    # 0. 계약 정보 로드 (가격 정밀도)
    print("\n[0] Fetching contract info (pricePlace)...")
    contracts = fetch_contracts(keys)
    print(f"  Loaded {len(contracts)} contracts")

    # 1. 열린 포지션 조회
    print("\n[1] Fetching open positions...")
    positions = get_all_positions(keys)
    if not positions:
        print("  No open positions found.")
        return

    print(f"  Found {len(positions)} open positions")

    # 2. 기존 TP/SL 주문 조회
    print("\n[2] Checking existing TP/SL orders...")
    pending_orders = get_pending_tpsl(keys)

    # 심볼+holdSide별 기존 SL 존재 여부
    has_sl = set()
    for order in pending_orders:
        plan_type = order.get("planType", "")
        sym = order.get("symbol", "")
        side = order.get("holdSide", "")
        if "loss" in plan_type.lower() or "sl" in plan_type.lower():
            has_sl.add(f"{sym}:{side}")

    print(f"  Existing SL orders: {len(has_sl)}")

    # 3. SL 없는 포지션에 백업 SL 적용
    print(f"\n[3] Applying backup SL ({sl_pct*100:.1f}%)...")
    applied = 0
    skipped = 0
    failed = 0
    already_has_sl = 0

    for pos in positions:
        symbol = pos.get("symbol", "")
        hold_side = pos.get("holdSide", "")
        size_str = pos.get("total", pos.get("available", "0"))
        size = float(size_str) if size_str else 0
        avg_price_str = pos.get("openPriceAvg", pos.get("averageOpenPrice", "0"))
        avg_price = float(avg_price_str) if avg_price_str else 0
        unrealized_pnl = float(pos.get("unrealizedPL", 0))

        if size <= 0 or avg_price <= 0:
            continue

        key = f"{symbol}:{hold_side}"

        # 이미 SL 있으면 스킵
        if key in has_sl:
            already_has_sl += 1
            continue

        # 백업 SL 가격 계산
        if hold_side == "long":
            sl_price = avg_price * (1.0 - sl_pct)
        else:
            sl_price = avg_price * (1.0 + sl_pct)

        print(f"\n  {symbol} ({hold_side}) | entry={avg_price:.4f} | size={size} | uPnL={unrealized_pnl:.2f}")
        print(f"    → SL target: {sl_price:.4f} ({sl_pct*100:.1f}% from entry)")

        # rate limit: 200ms per request
        if not dry_run:
            time.sleep(0.25)

        ok = place_tpsl_order(keys, symbol, sl_price, hold_side, size, contracts, dry_run=dry_run)
        if ok:
            applied += 1
        else:
            failed += 1

    # 4. 요약
    print(f"\n{'='*60}")
    print(f"  SUMMARY")
    print(f"  Total positions:   {len(positions)}")
    print(f"  Already has SL:    {already_has_sl}")
    print(f"  Backup SL applied: {applied}")
    print(f"  Failed:            {failed}")
    print(f"  Mode:              {mode_str}")
    print(f"{'='*60}")

    if dry_run and applied > 0:
        print(f"\n  → Run with --execute to apply these {applied} SL orders for real")

if __name__ == "__main__":
    main()

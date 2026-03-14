#!/usr/bin/env python3
"""Bitget 실제 상태 검증 스크립트 — 포지션, TP/SL, 주문, 고아 포지션 체크"""
import hmac, hashlib, base64, time, json, urllib.request, ssl, os

# Load config
cfg_path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'config', 'config.json')
with open(cfg_path) as f:
    cfg = json.load(f)

ak = cfg['api_key']
sk = cfg['api_secret']
pp = cfg['api_passphrase']

def sign_req(method, path, body=''):
    ts = str(int(time.time() * 1000))
    msg = ts + method + path + body
    sig = base64.b64encode(hmac.new(sk.encode(), msg.encode(), hashlib.sha256).digest()).decode()
    return ts, sig

def api_get(path):
    ts, sig = sign_req('GET', path)
    headers = {
        'ACCESS-KEY': ak, 'ACCESS-SIGN': sig, 'ACCESS-TIMESTAMP': ts,
        'ACCESS-PASSPHRASE': pp, 'Content-Type': 'application/json', 'locale': 'en-US'
    }
    ctx = ssl.create_default_context()
    req = urllib.request.Request(f'https://api.bitget.com{path}', headers=headers)
    with urllib.request.urlopen(req, context=ctx) as resp:
        return json.loads(resp.read())

print("=" * 60)
print("1. ACCOUNT BALANCE")
print("=" * 60)
try:
    acc = api_get('/api/v2/mix/account/account?productType=USDT-FUTURES&symbol=BTCUSDT')
    if acc.get('code') == '00000':
        d = acc['data']
        print(f"   Available:  {d.get('available','?')}")
        print(f"   Equity:     {d.get('accountEquity','?')}")
        print(f"   uPnL:       {d.get('unrealizedPL','?')}")
        print(f"   Margin:     {d.get('locked','?')}")
except Exception as e:
    print(f"   ERROR: {e}")

print("\n" + "=" * 60)
print("2. OPEN POSITIONS (Bitget actual)")
print("=" * 60)
try:
    pos = api_get('/api/v2/mix/position/all-position?productType=USDT-FUTURES&marginCoin=USDT')
    if pos.get('code') == '00000':
        positions = pos['data']
        print(f"   Total positions: {len(positions)}")

        no_tp = []
        no_sl = []
        with_tp = 0
        with_sl = 0
        total_margin = 0
        total_upnl = 0

        for p in positions:
            sym = p.get('symbol','?').replace('USDT','')
            side = p.get('holdSide','?')
            margin = float(p.get('margin','0') or '0')
            upnl = float(p.get('unrealizedPL','0') or '0')
            total_margin += margin
            total_upnl += upnl

            # TP/SL check via position fields
            tp = p.get('presetStopSurplusPrice','') or ''
            sl = p.get('presetStopLossPrice','') or ''

            if tp and float(tp) > 0:
                with_tp += 1
            else:
                no_tp.append(f"{sym}({side[0]})")
            if sl and float(sl) > 0:
                with_sl += 1
            else:
                no_sl.append(f"{sym}({side[0]})")

        print(f"   Total Margin: ${total_margin:.2f}")
        print(f"   Total uPnL:   ${total_upnl:.4f}")
        print(f"   With TP: {with_tp}/{len(positions)}")
        print(f"   With SL: {with_sl}/{len(positions)}")
        if no_tp:
            print(f"   !! NO TP ({len(no_tp)}): {', '.join(no_tp[:15])}{'...' if len(no_tp)>15 else ''}")
        if no_sl:
            print(f"   !! NO SL ({len(no_sl)}): {', '.join(no_sl[:15])}{'...' if len(no_sl)>15 else ''}")
except Exception as e:
    print(f"   ERROR: {e}")

print("\n" + "=" * 60)
print("3. PENDING TRIGGER ORDERS (TP/SL orders)")
print("=" * 60)
try:
    orders = api_get('/api/v2/mix/order/orders-plan-pending?productType=USDT-FUTURES')
    if orders.get('code') == '00000':
        data = orders.get('data', {})
        ords = data.get('entrustedList', []) if isinstance(data, dict) else data
        if isinstance(ords, list):
            print(f"   Total pending trigger orders: {len(ords)}")
            plan_types = {}
            for o in ords:
                pt = o.get('planType','unknown')
                plan_types[pt] = plan_types.get(pt, 0) + 1
            for pt, cnt in sorted(plan_types.items()):
                print(f"   - {pt}: {cnt}")
        else:
            print(f"   Data format: {type(data)}")
except Exception as e:
    print(f"   ERROR: {e}")

print("\n" + "=" * 60)
print("4. RECENT FILLS (last 10)")
print("=" * 60)
try:
    fills = api_get('/api/v2/mix/order/fills?productType=USDT-FUTURES&limit=10')
    if fills.get('code') == '00000':
        data = fills.get('data', {})
        fill_list = data.get('fillList', []) if isinstance(data, dict) else data
        if isinstance(fill_list, list):
            for f in fill_list[:10]:
                sym = f.get('symbol','?').replace('USDT','')
                side = f.get('side','?')
                sz = f.get('baseVolume','?')
                price = f.get('price','?')
                fee = f.get('fee','?')
                print(f"   {sym:12s} {side:5s} sz={sz:>12s} @{price:>12s} fee={fee}")
        else:
            print(f"   Data format: {type(data)}")
except Exception as e:
    print(f"   ERROR: {e}")

# 5. Compare with internal state
print("\n" + "=" * 60)
print("5. INTERNAL vs EXCHANGE COMPARISON")
print("=" * 60)
try:
    state_path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'data', 'state.json')
    if os.path.exists(state_path):
        with open(state_path) as f:
            state = json.load(f)
        internal_positions = state.get('positions', {})
        print(f"   Internal positions: {len(internal_positions)}")
        print(f"   Exchange positions: {len(positions)}")

        # Find orphans: in internal but not on exchange
        exchange_keys = set()
        for p in positions:
            sym = p.get('symbol','')
            side = p.get('holdSide','')
            exchange_keys.add(f"{sym}_{side}")

        orphans = []
        for key, pos_data in internal_positions.items():
            sym = pos_data.get('symbol','')
            side = 'long' if pos_data.get('side','') == 'buy' else 'short'
            ek = f"{sym}USDT_{side}"
            if ek not in exchange_keys:
                orphans.append(f"{sym}({side[0]})")

        if orphans:
            print(f"   !! ORPHANS in internal ({len(orphans)}): {', '.join(orphans[:15])}")
        else:
            print(f"   OK - No orphan positions")
    else:
        print(f"   state.json not found at {state_path}")
except Exception as e:
    print(f"   ERROR: {e}")

print("\n" + "=" * 60)
print("CHECK COMPLETE")
print("=" * 60)

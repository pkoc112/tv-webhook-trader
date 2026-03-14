#!/usr/bin/env python3
"""Bitget 실제 상태 검증 스크립트 — 포지션, TP/SL(preset+trigger), 주문, 고아 포지션 체크"""
import hmac, hashlib, base64, time, json, urllib.request, ssl, os

# Load config
cfg_path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'config', 'config.json')
with open(cfg_path) as f:
    cfg = json.load(f)

# API keys: either inline or in separate file
if 'api_key' in cfg:
    ak, sk, pp = cfg['api_key'], cfg['api_secret'], cfg.get('api_passphrase', cfg.get('passphrase',''))
elif 'api_keys_path' in cfg:
    keys_path = os.path.join(os.path.dirname(cfg_path), cfg['api_keys_path'].replace('config/',''))
    with open(keys_path) as f2:
        keys = json.load(f2)
    ak, sk, pp = keys['api_key'], keys['api_secret'], keys.get('passphrase', keys.get('api_passphrase',''))
else:
    raise RuntimeError("No API keys found in config")

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

# ============================================================
print("=" * 60)
print("1. ACCOUNT BALANCE")
print("=" * 60)
try:
    acc = api_get('/api/v2/mix/account/account?symbol=BTCUSDT&productType=USDT-FUTURES&marginCoin=USDT')
    if acc.get('code') == '00000':
        d = acc['data']
        print(f"   Available:  {d.get('available','?')}")
        print(f"   Equity:     {d.get('accountEquity','?')}")
        print(f"   uPnL:       {d.get('unrealizedPL','?')}")
        print(f"   Margin:     {d.get('locked','?')}")
except Exception as e:
    print(f"   ERROR: {e}")

# ============================================================
print("\n" + "=" * 60)
print("2. OPEN POSITIONS (Bitget actual)")
print("=" * 60)
positions = []
try:
    pos = api_get('/api/v2/mix/position/all-position?productType=USDT-FUTURES&marginCoin=USDT')
    if pos.get('code') == '00000':
        positions = pos['data']
        print(f"   Total positions: {len(positions)}")

        total_margin = 0
        total_upnl = 0
        for p in positions:
            margin = float(p.get('margin','0') or '0')
            upnl = float(p.get('unrealizedPL','0') or '0')
            total_margin += margin
            total_upnl += upnl

        print(f"   Total Margin: ${total_margin:.2f}")
        print(f"   Total uPnL:   ${total_upnl:.4f}")
except Exception as e:
    print(f"   ERROR: {e}")

# ============================================================
print("\n" + "=" * 60)
print("3. PENDING TRIGGER ORDERS (TP/SL)")
print("=" * 60)
all_trigger_orders = []
try:
    # Try multiple planType values and unfiltered
    for plan_type in ['profit_loss', 'normal_plan', 'pos_profit', 'pos_loss', None]:
        try:
            path = '/api/v2/mix/order/orders-plan-pending?productType=USDT-FUTURES&limit=100'
            if plan_type:
                path += f'&planType={plan_type}'
            resp = api_get(path)
            if resp.get('code') == '00000':
                data = resp.get('data', {})
                ords = data.get('entrustedList', []) if isinstance(data, dict) else (data if isinstance(data, list) else [])
                if ords:
                    all_trigger_orders.extend(ords)
                    label = plan_type or 'all'
                    print(f"   Found {len(ords)} orders ({label})")
        except Exception as e:
            pass  # silently skip failed queries
        time.sleep(0.12)

    print(f"   Total pending trigger orders: {len(all_trigger_orders)}")
    plan_types = {}
    for o in all_trigger_orders:
        pt = o.get('planType', 'unknown')
        plan_types[pt] = plan_types.get(pt, 0) + 1
    for pt, cnt in sorted(plan_types.items()):
        print(f"   - {pt}: {cnt}")
except Exception as e:
    print(f"   ERROR: {e}")

# ============================================================
print("\n" + "=" * 60)
print("4. TP/SL PROTECTION CHECK (preset + trigger orders)")
print("=" * 60)
try:
    # Build trigger order map
    trigger_map = {}  # "BTCUSDT_long" → {"tp": bool, "sl": bool}
    for o in all_trigger_orders:
        sym = o.get('symbol', '')
        hold = o.get('holdSide', '')
        pt = o.get('planType', '')
        key = f"{sym}_{hold}"
        if key not in trigger_map:
            trigger_map[key] = {'tp': False, 'sl': False}
        if pt in ('profit_plan', 'pos_profit'):
            trigger_map[key]['tp'] = True
        elif pt in ('loss_plan', 'pos_loss'):
            trigger_map[key]['sl'] = True

    no_tp = []
    no_sl = []
    with_tp = 0
    with_sl = 0

    for p in positions:
        sym = p.get('symbol', '?')
        hold = p.get('holdSide', '?')
        short_sym = sym.replace('USDT', '')
        key = f"{sym}_{hold}"

        # Check preset fields
        preset_tp = p.get('presetStopSurplusPrice', '') or ''
        preset_sl = p.get('presetStopLossPrice', '') or ''
        has_preset_tp = bool(preset_tp and float(preset_tp) > 0)
        has_preset_sl = bool(preset_sl and float(preset_sl) > 0)

        # Check trigger orders
        trig = trigger_map.get(key, {'tp': False, 'sl': False})

        has_tp = has_preset_tp or trig['tp']
        has_sl = has_preset_sl or trig['sl']

        if has_tp:
            with_tp += 1
        else:
            no_tp.append(f"{short_sym}({hold[0]})")

        if has_sl:
            with_sl += 1
        else:
            no_sl.append(f"{short_sym}({hold[0]})")

    print(f"   With TP: {with_tp}/{len(positions)}")
    print(f"   With SL: {with_sl}/{len(positions)}")
    if no_tp:
        print(f"   !! NO TP ({len(no_tp)}): {', '.join(no_tp[:20])}{'...' if len(no_tp)>20 else ''}")
    if no_sl:
        print(f"   !! NO SL ({len(no_sl)}): {', '.join(no_sl[:20])}{'...' if len(no_sl)>20 else ''}")
    if not no_tp and not no_sl:
        print(f"   ✅ All positions protected!")
except Exception as e:
    print(f"   ERROR: {e}")

# ============================================================
print("\n" + "=" * 60)
print("5. RECENT FILLS (last 10)")
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
except Exception as e:
    print(f"   ERROR: {e}")

# ============================================================
print("\n" + "=" * 60)
print("6. INTERNAL vs EXCHANGE COMPARISON")
print("=" * 60)
try:
    state_path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'data', 'state.json')
    if os.path.exists(state_path):
        with open(state_path) as f:
            state = json.load(f)
        internal_positions = state.get('positions', {})
        print(f"   Internal positions: {len(internal_positions)}")
        print(f"   Exchange positions: {len(positions)}")

        # Exchange keys: "BTCUSDT_long", "ETHUSDT_short"
        exchange_keys = set()
        for p in positions:
            sym = p.get('symbol', '')
            side = p.get('holdSide', '')
            exchange_keys.add(f"{sym}_{side}")

        # Check orphans (in internal but not on exchange)
        orphans = []
        for key, pos_data in internal_positions.items():
            sym = pos_data.get('symbol', '')
            # Internal state may store "BTC" or "BTCUSDT"
            if not sym.endswith('USDT'):
                sym = sym + 'USDT'
            side_raw = pos_data.get('side', '')
            if side_raw in ('buy', 'long'):
                hold = 'long'
            elif side_raw in ('sell', 'short'):
                hold = 'short'
            else:
                hold = side_raw
            ek = f"{sym}_{hold}"
            if ek not in exchange_keys:
                orphans.append(f"{sym.replace('USDT','')}({hold[0] if hold else '?'})")

        # Check reverse orphans (on exchange but not in internal)
        internal_keys = set()
        for key, pos_data in internal_positions.items():
            sym = pos_data.get('symbol', '')
            if not sym.endswith('USDT'):
                sym = sym + 'USDT'
            side_raw = pos_data.get('side', '')
            if side_raw in ('buy', 'long'):
                hold = 'long'
            elif side_raw in ('sell', 'short'):
                hold = 'short'
            else:
                hold = side_raw
            internal_keys.add(f"{sym}_{hold}")

        reverse_orphans = []
        for p in positions:
            sym = p.get('symbol', '')
            hold = p.get('holdSide', '')
            ek = f"{sym}_{hold}"
            if ek not in internal_keys:
                reverse_orphans.append(f"{sym.replace('USDT','')}({hold[0]})")

        if orphans:
            print(f"   !! GHOST in internal ({len(orphans)}): {', '.join(orphans[:15])}")
        else:
            print(f"   ✓ No ghost positions in internal state")

        if reverse_orphans:
            print(f"   !! UNTRACKED on exchange ({len(reverse_orphans)}): {', '.join(reverse_orphans[:15])}")
        else:
            print(f"   ✓ All exchange positions tracked internally")
    else:
        print(f"   state.json not found at {state_path}")
except Exception as e:
    print(f"   ERROR: {e}")

print("\n" + "=" * 60)
print("CHECK COMPLETE")
print("=" * 60)

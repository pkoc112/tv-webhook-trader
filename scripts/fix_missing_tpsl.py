#!/usr/bin/env python3
"""
기존 포지션에 누락된 TP/SL을 소급 설정하는 스크립트.

동작:
1. 모든 오픈 포지션 조회
2. 모든 대기 중인 트리거 주문(TP/SL) 조회
3. 포지션별로 TP/SL 트리거 주문 존재 여부 확인
4. 누락된 경우 → 기본 SL(entry ±2%), TP(entry ±3%) 설정

사용법:
  python3 scripts/fix_missing_tpsl.py          # dry-run (기본)
  python3 scripts/fix_missing_tpsl.py --apply   # 실제 적용
"""
import hmac, hashlib, base64, time, json, urllib.request, ssl, os, sys

DRY_RUN = '--apply' not in sys.argv

# ---- Config & Auth ----
cfg_path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'config', 'config.json')
with open(cfg_path) as f:
    cfg = json.load(f)

if 'api_key' in cfg:
    ak, sk, pp = cfg['api_key'], cfg['api_secret'], cfg.get('api_passphrase', cfg.get('passphrase',''))
elif 'api_keys_path' in cfg:
    keys_path = os.path.join(os.path.dirname(cfg_path), cfg['api_keys_path'].replace('config/',''))
    with open(keys_path) as f2:
        keys = json.load(f2)
    ak, sk, pp = keys['api_key'], keys['api_secret'], keys.get('passphrase', keys.get('api_passphrase',''))
else:
    raise RuntimeError("No API keys found")

ctx = ssl.create_default_context()

def sign_req(method, path, body=''):
    ts = str(int(time.time() * 1000))
    msg = ts + method + path + body
    sig = base64.b64encode(hmac.new(sk.encode(), msg.encode(), hashlib.sha256).digest()).decode()
    return ts, sig

def api_call(method, path, body=''):
    ts, sig = sign_req(method, path, body)
    headers = {
        'ACCESS-KEY': ak, 'ACCESS-SIGN': sig, 'ACCESS-TIMESTAMP': ts,
        'ACCESS-PASSPHRASE': pp, 'Content-Type': 'application/json', 'locale': 'en-US'
    }
    url = f'https://api.bitget.com{path}'
    data = body.encode() if body else None
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    with urllib.request.urlopen(req, context=ctx) as resp:
        return json.loads(resp.read())

def api_get(path):
    return api_call('GET', path)

def api_post(path, body_dict):
    body_str = json.dumps(body_dict)
    return api_call('POST', path, body_str)

# ---- 1. Fetch all positions ----
print("=" * 70)
print(f"  FIX MISSING TP/SL  {'[DRY-RUN]' if DRY_RUN else '[APPLY MODE]'}")
print("=" * 70)

pos_resp = api_get('/api/v2/mix/position/all-position?productType=USDT-FUTURES&marginCoin=USDT')
if pos_resp.get('code') != '00000':
    print(f"ERROR: Cannot fetch positions: {pos_resp}")
    sys.exit(1)

positions = pos_resp['data']
print(f"\nTotal open positions: {len(positions)}")

# ---- 2. Fetch all pending trigger orders (paginate) ----
print("\nFetching pending trigger orders...")
all_trigger_orders = []
last_end_id = ''
page = 0
while True:
    page += 1
    path = '/api/v2/mix/order/orders-plan-pending?productType=USDT-FUTURES&limit=100'
    if last_end_id:
        path += f'&idLessThan={last_end_id}'
    resp = api_get(path)
    if resp.get('code') != '00000':
        print(f"  Warning: trigger orders fetch error (page {page}): {resp.get('msg','?')}")
        break
    data = resp.get('data', {})
    ords = data.get('entrustedList', []) if isinstance(data, dict) else (data if isinstance(data, list) else [])
    if not ords:
        break
    all_trigger_orders.extend(ords)
    last_end_id = data.get('endId', '') if isinstance(data, dict) else ''
    if not last_end_id or len(ords) < 100:
        break
    time.sleep(0.15)  # rate limit

print(f"Total pending trigger orders: {len(all_trigger_orders)}")

# ---- 3. Build trigger order map: symbol+holdSide → {has_tp, has_sl} ----
trigger_map = {}  # key: "BTCUSDT_long" → {"tp": bool, "sl": bool}
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

# ---- 4. Check each position ----
missing_tp = []
missing_sl = []
protected = 0

for p in positions:
    sym = p.get('symbol', '')
    hold = p.get('holdSide', '')
    key = f"{sym}_{hold}"
    entry = float(p.get('openPriceAvg', '0') or '0')
    size_str = p.get('total', p.get('available', '0'))
    size = float(size_str or '0')
    margin = float(p.get('margin', '0') or '0')

    # Check preset fields first
    preset_tp = p.get('presetStopSurplusPrice', '') or ''
    preset_sl = p.get('presetStopLossPrice', '') or ''
    has_preset_tp = bool(preset_tp and float(preset_tp) > 0)
    has_preset_sl = bool(preset_sl and float(preset_sl) > 0)

    # Check trigger orders
    trig = trigger_map.get(key, {'tp': False, 'sl': False})
    has_trigger_tp = trig['tp']
    has_trigger_sl = trig['sl']

    has_tp = has_preset_tp or has_trigger_tp
    has_sl = has_preset_sl or has_trigger_sl

    if has_tp and has_sl:
        protected += 1
        continue

    short_sym = sym.replace('USDT', '')
    info = {
        'symbol': sym, 'holdSide': hold, 'entry': entry, 'size': size,
        'margin': margin, 'short_sym': short_sym
    }
    if not has_tp:
        missing_tp.append(info)
    if not has_sl:
        missing_sl.append(info)

print(f"\nProtected (TP+SL): {protected}/{len(positions)}")
print(f"Missing TP: {len(missing_tp)}")
print(f"Missing SL: {len(missing_sl)}")

if not missing_tp and not missing_sl:
    print("\n✅ All positions are protected!")
    sys.exit(0)

# ---- 5. Fix missing TP/SL ----
# Default: SL = 2% adverse, TP = 3% favorable (conservative)
SL_PCT = 0.02
TP_PCT = 0.03

def calc_tpsl(entry, hold_side):
    """Calculate default TP/SL prices based on position direction."""
    if hold_side == 'long':
        tp = entry * (1 + TP_PCT)
        sl = entry * (1 - SL_PCT)
    else:  # short
        tp = entry * (1 - TP_PCT)
        sl = entry * (1 + SL_PCT)
    return round(tp, 6), round(sl, 6)

print(f"\n{'=' * 70}")
print(f"  FIXING: SL={SL_PCT*100}% / TP={TP_PCT*100}% from entry")
print(f"{'=' * 70}")

fixed_tp = 0
fixed_sl = 0
errors = 0

# Combine unique positions needing fixes
all_missing = {}
for info in missing_tp:
    key = f"{info['symbol']}_{info['holdSide']}"
    all_missing[key] = {**info, 'need_tp': True, 'need_sl': False}
for info in missing_sl:
    key = f"{info['symbol']}_{info['holdSide']}"
    if key in all_missing:
        all_missing[key]['need_sl'] = True
    else:
        all_missing[key] = {**info, 'need_tp': False, 'need_sl': True}

for i, (key, info) in enumerate(all_missing.items()):
    sym = info['symbol']
    hold = info['holdSide']
    entry = info['entry']
    size = info['size']
    tp_price, sl_price = calc_tpsl(entry, hold)

    status_parts = []
    if info.get('need_sl'):
        status_parts.append(f"SL@{sl_price:.4f}")
    if info.get('need_tp'):
        status_parts.append(f"TP@{tp_price:.4f}")

    print(f"  [{i+1}/{len(all_missing)}] {info['short_sym']:12s} {hold:5s} entry={entry:.4f} → {', '.join(status_parts)}", end='')

    if DRY_RUN:
        print("  [SKIP - dry run]")
        continue

    # Set SL first (more important)
    if info.get('need_sl'):
        try:
            sl_body = {
                "symbol": sym, "productType": "USDT-FUTURES",
                "marginMode": "crossed", "marginCoin": "USDT",
                "planType": "pos_loss", "triggerPrice": str(sl_price),
                "triggerType": "mark_price", "size": str(size),
                "holdSide": hold
            }
            resp = api_post('/api/v2/mix/order/place-tpsl-order', sl_body)
            if resp.get('code') == '00000':
                fixed_sl += 1
                print(" SL✓", end='')
            else:
                print(f" SL✗({resp.get('msg','')})", end='')
                errors += 1
        except Exception as e:
            print(f" SL✗({e})", end='')
            errors += 1
        time.sleep(0.12)  # rate limit

    # Set TP
    if info.get('need_tp'):
        try:
            tp_body = {
                "symbol": sym, "productType": "USDT-FUTURES",
                "marginMode": "crossed", "marginCoin": "USDT",
                "planType": "pos_profit", "triggerPrice": str(tp_price),
                "triggerType": "mark_price", "size": str(size),
                "holdSide": hold
            }
            resp = api_post('/api/v2/mix/order/place-tpsl-order', tp_body)
            if resp.get('code') == '00000':
                fixed_tp += 1
                print(" TP✓", end='')
            else:
                print(f" TP✗({resp.get('msg','')})", end='')
                errors += 1
        except Exception as e:
            print(f" TP✗({e})", end='')
            errors += 1
        time.sleep(0.12)

    print()

print(f"\n{'=' * 70}")
print(f"  RESULT: TP fixed={fixed_tp}, SL fixed={fixed_sl}, errors={errors}")
if DRY_RUN:
    print(f"  ⚠️  DRY-RUN mode. Run with --apply to actually fix.")
print(f"{'=' * 70}")

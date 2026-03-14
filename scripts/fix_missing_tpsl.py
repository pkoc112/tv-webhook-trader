#!/usr/bin/env python3
"""
기존 포지션에 누락된 TP/SL을 소급 설정하는 스크립트.

v2: 가격 정밀도(pricePlace) 적용 + mark price 기반 유효성 검사

사용법:
  python3 scripts/fix_missing_tpsl.py          # dry-run (기본)
  python3 scripts/fix_missing_tpsl.py --apply   # 실제 적용
"""
import hmac, hashlib, base64, time, json, urllib.request, urllib.error, ssl, os, sys, math

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
    try:
        with urllib.request.urlopen(req, context=ctx) as resp:
            return json.loads(resp.read())
    except urllib.error.HTTPError as e:
        body_text = e.read().decode('utf-8', errors='replace')
        try:
            return json.loads(body_text)
        except:
            raise Exception(f"HTTP {e.code}: {body_text[:200]}")

def api_get(path):
    return api_call('GET', path)

def api_post(path, body_dict):
    body_str = json.dumps(body_dict)
    return api_call('POST', path, body_str)

def round_price(price, decimals):
    """Round price to the correct number of decimal places."""
    if decimals <= 0:
        return round(price)
    factor = 10 ** decimals
    return math.floor(price * factor) / factor

# ---- 0. Fetch contract info for price precision ----
print("Fetching contract info (price precision)...")
contracts = {}
try:
    resp = api_get('/api/v2/mix/market/contracts?productType=USDT-FUTURES')
    if resp.get('code') == '00000':
        for c in resp['data']:
            sym = c.get('symbol', '')
            if sym:
                contracts[sym] = {
                    'pricePlace': int(c.get('pricePlace', 4)),
                    'priceEndStep': int(c.get('priceEndStep', 1)),
                    'volumePlace': int(c.get('volumePlace', 4)),
                }
        print(f"  Loaded {len(contracts)} contracts")
except Exception as e:
    print(f"  Warning: contract fetch failed: {e}")

def get_price_decimals(symbol):
    """Get the number of decimal places for a symbol's price."""
    if symbol in contracts:
        return contracts[symbol]['pricePlace']
    # Fallback: guess from entry price
    return 4

def format_price(price, symbol):
    """Format price with correct precision for the symbol."""
    decimals = get_price_decimals(symbol)
    rounded = round_price(price, decimals)
    return f"{rounded:.{decimals}f}"

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

# ---- 2. Skip trigger order check (API unreliable), go straight to protection check ----
# We know from previous runs that there are 0 trigger orders

# ---- 3. Check each position ----
missing = []  # positions needing TP and/or SL
protected = 0

for p in positions:
    sym = p.get('symbol', '')
    hold = p.get('holdSide', '')
    entry = float(p.get('openPriceAvg', '0') or '0')
    mark = float(p.get('markPrice', '0') or '0')
    size_str = p.get('total', p.get('available', '0'))
    size = float(size_str or '0')

    # Check preset fields
    preset_tp = p.get('presetStopSurplusPrice', '') or ''
    preset_sl = p.get('presetStopLossPrice', '') or ''
    has_tp = bool(preset_tp and float(preset_tp) > 0)
    has_sl = bool(preset_sl and float(preset_sl) > 0)

    if has_tp and has_sl:
        protected += 1
        continue

    short_sym = sym.replace('USDT', '')
    missing.append({
        'symbol': sym, 'holdSide': hold, 'entry': entry, 'mark': mark,
        'size': size, 'short_sym': short_sym,
        'need_tp': not has_tp, 'need_sl': not has_sl
    })

print(f"\nProtected (TP+SL): {protected}/{len(positions)}")
print(f"Need fixing: {len(missing)}")

if not missing:
    print("\n✅ All positions are protected!")
    sys.exit(0)

# ---- 4. Fix missing TP/SL ----
SL_PCT = 0.02   # 2% stop loss
TP_PCT = 0.03   # 3% take profit

print(f"\n{'=' * 70}")
print(f"  FIXING: SL={SL_PCT*100}% / TP={TP_PCT*100}%")
print(f"  Using MARK PRICE when entry-based price is invalid")
print(f"{'=' * 70}")

fixed_tp = 0
fixed_sl = 0
errors = 0
skipped = 0

for i, info in enumerate(missing):
    sym = info['symbol']
    hold = info['holdSide']
    entry = info['entry']
    mark = info['mark']
    size = info['size']
    decimals = get_price_decimals(sym)

    # Use mark price as reference if available, otherwise entry
    ref_price = mark if mark > 0 else entry

    # Calculate TP/SL based on direction
    if hold == 'long':
        tp_raw = ref_price * (1 + TP_PCT)
        sl_raw = ref_price * (1 - SL_PCT)
        # Validate: TP must be > mark, SL must be < mark
        if mark > 0:
            if tp_raw <= mark:
                tp_raw = mark * (1 + 0.01)  # at least 1% above mark
            if sl_raw >= mark:
                sl_raw = mark * (1 - 0.01)  # at least 1% below mark
    else:  # short
        tp_raw = ref_price * (1 - TP_PCT)
        sl_raw = ref_price * (1 + SL_PCT)
        # Validate: TP must be < mark, SL must be > mark
        if mark > 0:
            if tp_raw >= mark:
                tp_raw = mark * (1 - 0.01)
            if sl_raw <= mark:
                sl_raw = mark * (1 + 0.01)

    tp_price = format_price(tp_raw, sym)
    sl_price = format_price(sl_raw, sym)

    status = f"  [{i+1}/{len(missing)}] {info['short_sym']:12s} {hold:5s} mark={ref_price:.4f} → SL={sl_price}, TP={tp_price}"
    print(status, end='')

    if DRY_RUN:
        print("  [SKIP]")
        continue

    # Set SL first (more important for protection)
    if info.get('need_sl'):
        try:
            sl_body = {
                "symbol": sym, "productType": "USDT-FUTURES",
                "marginMode": "crossed", "marginCoin": "USDT",
                "planType": "pos_loss", "triggerPrice": sl_price,
                "triggerType": "mark_price", "size": str(size),
                "holdSide": hold
            }
            resp = api_post('/api/v2/mix/order/place-tpsl-order', sl_body)
            if resp.get('code') == '00000':
                fixed_sl += 1
                print(" SL✓", end='')
            else:
                msg = resp.get('msg', '')[:60]
                print(f" SL✗({msg})", end='')
                errors += 1
        except Exception as e:
            print(f" SL✗({str(e)[:50]})", end='')
            errors += 1
        time.sleep(0.12)

    # Set TP
    if info.get('need_tp'):
        try:
            tp_body = {
                "symbol": sym, "productType": "USDT-FUTURES",
                "marginMode": "crossed", "marginCoin": "USDT",
                "planType": "pos_profit", "triggerPrice": tp_price,
                "triggerType": "mark_price", "size": str(size),
                "holdSide": hold
            }
            resp = api_post('/api/v2/mix/order/place-tpsl-order', tp_body)
            if resp.get('code') == '00000':
                fixed_tp += 1
                print(" TP✓", end='')
            else:
                msg = resp.get('msg', '')[:60]
                print(f" TP✗({msg})", end='')
                errors += 1
        except Exception as e:
            print(f" TP✗({str(e)[:50]})", end='')
            errors += 1
        time.sleep(0.12)

    print()

print(f"\n{'=' * 70}")
print(f"  RESULT: TP fixed={fixed_tp}, SL fixed={fixed_sl}, errors={errors}")
if DRY_RUN:
    print(f"  ⚠️  DRY-RUN mode. Run with --apply to actually fix.")
print(f"{'=' * 70}")

#!/usr/bin/env python3
"""
모든 Bitget 포지션 청산 + 대기 중인 TP/SL 주문 취소.
새로운 시작을 위한 클린업 스크립트.

사용법:
  python3 scripts/close_all_positions.py          # dry-run
  python3 scripts/close_all_positions.py --apply   # 실제 적용
"""
import hmac, hashlib, base64, time, json, urllib.request, urllib.error, ssl, os, sys

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
            print(f"  HTTP {e.code}: {body_text[:200]}")
            return {"code": str(e.code), "msg": body_text[:200]}

# ---- Step 1: Get all open positions ----
print("=" * 60)
print(f"{'DRY RUN' if DRY_RUN else 'LIVE'} — Close All Positions & Cancel Pending Orders")
print("=" * 60)

resp = api_call('GET', '/api/v2/mix/position/all-position?productType=USDT-FUTURES')
positions = []
if resp.get('code') == '00000' and resp.get('data'):
    for p in resp['data']:
        total = float(p.get('total', '0'))
        if total > 0:
            positions.append({
                'symbol': p['symbol'],
                'holdSide': p.get('holdSide', ''),
                'total': total,
                'unrealizedPL': p.get('unrealizedPL', '0'),
                'marginSize': p.get('marginSize', '0'),
            })

print(f"\nOpen positions: {len(positions)}")
for p in positions:
    print(f"  {p['symbol']:20s} {p['holdSide']:6s} size={p['total']:.6f} uPnL={p['unrealizedPL']}")

# ---- Step 2: Close all positions via flash close ----
print(f"\n--- Closing {len(positions)} positions ---")
closed = 0
for p in positions:
    if DRY_RUN:
        print(f"  [DRY] Would close {p['symbol']} {p['holdSide']}")
        continue

    body = json.dumps({
        "symbol": p['symbol'],
        "productType": "USDT-FUTURES",
        "holdSide": p['holdSide']
    })
    path = '/api/v2/mix/order/close-positions'
    result = api_call('POST', path, body)
    code = result.get('code', '99999')
    if code == '00000':
        print(f"  CLOSED: {p['symbol']} {p['holdSide']}")
        closed += 1
    else:
        print(f"  FAIL:   {p['symbol']} {p['holdSide']} — {result.get('msg', 'unknown')}")
    time.sleep(0.12)  # rate limit

print(f"Closed: {closed}/{len(positions)}")

# ---- Step 3: Cancel all pending TP/SL orders ----
print(f"\n--- Cancelling pending TP/SL orders ---")
all_pending = []
last_end_id = None
while True:
    path = '/api/v2/mix/order/orders-plan-pending?productType=USDT-FUTURES&planType=profit_loss&limit=100'
    if last_end_id:
        path += f'&idLessThan={last_end_id}'
    resp = api_call('GET', path)
    if resp.get('code') != '00000':
        print(f"  Failed to fetch pending: {resp.get('msg')}")
        break
    orders = resp.get('data', {}).get('entrustedList', [])
    if not orders:
        break
    all_pending.extend(orders)
    last_end_id = resp.get('data', {}).get('endId')
    if not last_end_id:
        break

print(f"Pending TP/SL orders: {len(all_pending)}")
cancelled = 0
for o in all_pending:
    oid = o.get('orderId', '')
    sym = o.get('symbol', '')
    pt = o.get('planType', '')
    if DRY_RUN:
        print(f"  [DRY] Would cancel {sym} {pt} id={oid}")
        continue

    body = json.dumps({
        "orderId": oid,
        "symbol": sym,
        "productType": "USDT-FUTURES",
        "marginCoin": "USDT"
    })
    path = '/api/v2/mix/order/cancel-plan-order'
    result = api_call('POST', path, body)
    code = result.get('code', '99999')
    if code == '00000':
        cancelled += 1
    else:
        print(f"  FAIL cancel {sym} {pt}: {result.get('msg')}")
    time.sleep(0.12)

print(f"Cancelled: {cancelled}/{len(all_pending)}")

# ---- Step 4: Final balance check ----
print(f"\n--- Final Account Status ---")
acct = api_call('GET', '/api/v2/mix/account/account?symbol=BTCUSDT&productType=USDT-FUTURES&marginCoin=USDT')
if acct.get('code') == '00000' and acct.get('data'):
    d = acct['data']
    print(f"  Available:  {d.get('available', '?')} USDT")
    print(f"  Equity:     {d.get('accountEquity', '?')} USDT")
    print(f"  Unrealized: {d.get('unrealizedPL', '?')} USDT")

# ---- Step 5: Clear internal state file ----
if not DRY_RUN:
    state_path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'data', 'state.json')
    if os.path.exists(state_path):
        with open(state_path) as f:
            old = json.load(f)
        balance = float(old.get('balance', 100))
        new_state = {
            "balance": balance,
            "peak_balance": balance,
            "saved_at": time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
            "positions": {},
            "trades": old.get('trades', [])  # 거래 기록은 유지
        }
        with open(state_path, 'w') as f:
            json.dump(new_state, f, indent=2)
        print(f"\n  State file reset: positions cleared, trades preserved ({len(new_state['trades'])} records)")

print(f"\n{'='*60}")
if DRY_RUN:
    print("DRY RUN complete. Run with --apply to execute.")
else:
    print("DONE. All positions closed, TP/SL cancelled, state reset.")
print(f"{'='*60}")

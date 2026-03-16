#!/usr/bin/env python3
"""
Telegram Bot Service for tv-webhook-trader v2.0
- Log monitoring & trade notifications (one-way alerts)
- Interactive bot commands for mobile remote control

Environment variables:
    TELEGRAM_BOT_TOKEN      - Bot token from @BotFather
    TELEGRAM_CHAT_ID        - Chat/group ID for notifications
    ALERT_SERVICE_NAME      - systemd service name (default: tv-webhook-trader)
    DASHBOARD_URL           - Dashboard API base URL (default: http://127.0.0.1:5000)
"""

import os
import re
import sys
import time
import signal
import subprocess
import threading
import logging
import json
import hmac
import hashlib
import base64
from collections import deque
from urllib.request import Request, urlopen
from urllib.error import URLError, HTTPError
from datetime import datetime, timezone, timedelta

# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------
BOT_TOKEN = os.environ.get("TELEGRAM_BOT_TOKEN", "")
CHAT_ID = os.environ.get("TELEGRAM_CHAT_ID", "")
SERVICE_NAME = os.environ.get("ALERT_SERVICE_NAME", "tv-webhook-trader")
DASHBOARD_URL = os.environ.get("DASHBOARD_URL", "http://127.0.0.1:5000")
CONFIG_PATH = os.environ.get("CONFIG_PATH", "/home/ubuntu/tv-webhook-trader/config/config.json")

RATE_LIMIT_MAX = 25
RATE_LIMIT_WINDOW = 60
BATCH_DELAY = 2.0
RECONNECT_DELAY = 5
POLL_INTERVAL = 1.5      # Telegram getUpdates poll interval
KST = timezone(timedelta(hours=9))

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [tg-bot] %(levelname)s %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S",
)
log = logging.getLogger("tg-bot")

# ---------------------------------------------------------------------------
# Patterns to match (실제 체결/에러만)
# ---------------------------------------------------------------------------
IMPORTANT_PATTERNS = [
    re.compile(r"\[TRADE\]"),                 # 구조화된 거래 이벤트
    re.compile(r"EMERGENCY CLOSE"),           # 긴급 청산
    re.compile(r"CIRCUIT.?BREAKER", re.IGNORECASE),  # 서킷브레이커
]
# 제외 패턴 (레버리지 초기화 등 불필요한 에러)
IGNORE_PATTERNS = [
    re.compile(r"Leverage failed.*429"),
    re.compile(r"Leverage set:"),
    re.compile(r"Setting leverage:"),
    re.compile(r"\[REST\].*Leverage"),
    re.compile(r"\[Sync\]"),
    re.compile(r"\[Periodic\]"),
]

def is_important(line: str) -> bool:
    if any(p.search(line) for p in IGNORE_PATTERNS):
        return False
    return any(p.search(line) for p in IMPORTANT_PATTERNS)

# ---------------------------------------------------------------------------
# [TRADE] 로그 → 한국어 문장 변환
# ---------------------------------------------------------------------------
RE_ENTRY = re.compile(
    r'\[TRADE\] (ENTRY|REENTRY) (LONG|SHORT) (\w+) (\d+)x @ ([\d.]+) \$([\d.]+) TP1=([\d.]+) SL=([\d.]+)')
RE_TP = re.compile(
    r'\[TRADE\] (TP\d) (\w+) ([-\d.]+) @ ([\d.]+) \((\d+)%\)')
RE_SL = re.compile(
    r'\[TRADE\] SL (\w+) ([-\d.]+) @ ([\d.]+)')

def format_line(line: str) -> str:
    m = re.match(r"^.*?tv-webhook-trader\[\d+\]:\s*(.+)$", line)
    msg = m.group(1) if m else line.strip()

    # 진입/재진입 체결
    m = RE_ENTRY.search(msg)
    if m:
        entry_type, side, sym, lev, price, usd, tp1, sl = m.groups()
        sym = sym.replace("USDT", "")
        side_kr = "롱" if side == "LONG" else "숏"
        is_reentry = entry_type == "REENTRY"
        if is_reentry:
            emoji = "🔄"
            type_kr = "재진입"
        else:
            emoji = "🟢" if side == "LONG" else "🔴"
            type_kr = "진입"
        tp1_s = tp1 if float(tp1) > 0 else "-"
        sl_s = sl if float(sl) > 0 else "-"
        # TP1까지 예상 수익 계산
        tp1_info = ""
        if float(tp1) > 0 and float(price) > 0:
            tp1_dist_pct = abs(float(tp1) - float(price)) / float(price) * 100 * int(lev)
            tp1_est_usd = float(usd) * tp1_dist_pct / 100
            tp1_info = f"\n📊 TP1 예상: +${tp1_est_usd:.2f} ({tp1_dist_pct:.1f}%)"
        # SL 예상 손실
        sl_info = ""
        if float(sl) > 0 and float(price) > 0:
            sl_dist_pct = abs(float(sl) - float(price)) / float(price) * 100 * int(lev)
            sl_est_usd = float(usd) * sl_dist_pct / 100
            sl_info = f"\n🛑 SL 예상: -${sl_est_usd:.2f} ({sl_dist_pct:.1f}%)"
        return (
            f"{emoji} <b>{sym}</b> {side_kr} {type_kr} {lev}x @ {price}\n"
            f"💰 ${usd} | TP1: {tp1_s} | SL: {sl_s}"
            f"{tp1_info}{sl_info}"
        )

    # TP 익절 (부분/전체)
    m = RE_TP.search(msg)
    if m:
        tp, sym, pnl, price, pct = m.groups()
        sym = sym.replace("USDT", "")
        pnl_f = float(pnl)
        pct_i = int(pct)
        if tp == "TP3" or pct_i >= 100:
            emoji = "🏆"
            tp_desc = "전체 익절"
        elif tp == "TP2":
            emoji = "🎯🎯"
            tp_desc = f"부분 익절 ({pct}%)"
        else:
            emoji = "🎯"
            tp_desc = f"부분 익절 ({pct}%)"
        pnl_emoji = "💚" if pnl_f >= 0 else "❤️"
        return (
            f"{emoji} <b>{sym}</b> {tp} {tp_desc} @ {price}\n"
            f"{pnl_emoji} PnL: {pnl_f:+.4f} USDT"
        )

    # SL 손절
    m = RE_SL.search(msg)
    if m:
        sym, pnl, price = m.groups()
        sym = sym.replace("USDT", "")
        pnl_f = float(pnl)
        return (
            f"🛑 <b>{sym}</b> 손절 @ {price}\n"
            f"💔 PnL: {pnl_f:+.4f} USDT"
        )

    # FAIL/ERROR/EMERGENCY 등 기타
    lo = msg.upper()
    if "EMERGENCY" in lo:
        emoji = "\U0001f6a8"
    elif "CIRCUIT" in lo and "BREAKER" in lo:
        emoji = "\U0001f6a8"
    elif "FAIL" in lo or "ERROR" in lo:
        emoji = "\u26a0\ufe0f"
    else:
        emoji = "\U0001f4ac"
    return f"{emoji} <code>{msg}</code>"

# ---------------------------------------------------------------------------
# Rate limiter
# ---------------------------------------------------------------------------
class RateLimiter:
    def __init__(self, max_calls: int, window: float):
        self._max = max_calls
        self._window = window
        self._timestamps: deque = deque()
        self._lock = threading.Lock()

    def acquire(self) -> bool:
        now = time.monotonic()
        with self._lock:
            while self._timestamps and self._timestamps[0] < now - self._window:
                self._timestamps.popleft()
            if len(self._timestamps) >= self._max:
                return False
            self._timestamps.append(now)
            return True

    def wait(self):
        while not self.acquire():
            time.sleep(1)

rate_limiter = RateLimiter(RATE_LIMIT_MAX, RATE_LIMIT_WINDOW)

# ---------------------------------------------------------------------------
# Telegram API helpers
# ---------------------------------------------------------------------------
def tg_api(method: str, payload: dict = None, timeout: int = 10):
    url = f"https://api.telegram.org/bot{BOT_TOKEN}/{method}"
    data = json.dumps(payload).encode("utf-8") if payload else None
    req = Request(url, data=data, headers={"Content-Type": "application/json"})
    try:
        resp = urlopen(req, timeout=timeout)
        return json.loads(resp.read().decode())
    except Exception as e:
        log.warning("tg_api %s failed: %s", method, e)
        return None

def send_telegram(text: str, chat_id: str = None, reply_to: int = None):
    if not BOT_TOKEN:
        print(text, flush=True)
        return
    rate_limiter.wait()
    payload = {
        "chat_id": chat_id or CHAT_ID,
        "text": text,
        "parse_mode": "HTML",
        "disable_web_page_preview": True,
    }
    if reply_to:
        payload["reply_to_message_id"] = reply_to
    for attempt in range(3):
        result = tg_api("sendMessage", payload)
        if result and result.get("ok"):
            return
        time.sleep(2 ** attempt)
    log.error("Failed to send message after 3 attempts")

# ---------------------------------------------------------------------------
# Dashboard API helper
# ---------------------------------------------------------------------------
def _get_dashboard_auth():
    """config.json에서 dashboard_token 읽어 Basic Auth 헤더 생성"""
    try:
        with open(CONFIG_PATH) as f:
            cfg = json.load(f)
        token = cfg.get("dashboard_token", "")
        if token:
            return "Basic " + base64.b64encode(token.encode()).decode()
    except Exception:
        pass
    return ""

_dash_auth = None

def api_get(path: str):
    global _dash_auth
    if _dash_auth is None:
        _dash_auth = _get_dashboard_auth()
    try:
        url = f"{DASHBOARD_URL}{path}"
        headers = {"Accept": "application/json"}
        if _dash_auth:
            headers["Authorization"] = _dash_auth
        req = Request(url, headers=headers)
        resp = urlopen(req, timeout=5)
        return json.loads(resp.read().decode())
    except Exception as e:
        log.warning("API %s failed: %s", path, e)
        return None

# ---------------------------------------------------------------------------
# Bitget API helper (for close positions)
# ---------------------------------------------------------------------------
def load_exchange_keys():
    try:
        with open(CONFIG_PATH) as f:
            cfg = json.load(f)
        keys_path = cfg.get("api_keys_path", "")
        # 상대경로이면 프로젝트 루트 기준으로 resolve
        # api_keys_path="config/api_keys.json" → 프로젝트 루트(config의 상위)에서 해석
        if keys_path and not os.path.isabs(keys_path):
            project_root = os.path.dirname(os.path.dirname(CONFIG_PATH))
            keys_path = os.path.join(project_root, keys_path)
        with open(keys_path) as f:
            return json.load(f)
    except Exception as e:
        log.error("Failed to load exchange keys: %s", e)
        return None

def bitget_get(path: str):
    """Bitget GET API 호출"""
    keys = load_exchange_keys()
    if not keys:
        return None
    ts = str(int(time.time() * 1000))
    msg = ts + "GET" + path
    sig = base64.b64encode(
        hmac.new(keys["api_secret"].encode(), msg.encode(), hashlib.sha256).digest()
    ).decode()
    headers = {
        "ACCESS-KEY": keys["api_key"],
        "ACCESS-SIGN": sig,
        "ACCESS-TIMESTAMP": ts,
        "ACCESS-PASSPHRASE": keys["passphrase"],
        "Content-Type": "application/json",
        "locale": "en-US",
    }
    req = Request("https://api.bitget.com" + path, headers=headers)
    try:
        resp = urlopen(req, timeout=10)
        return json.loads(resp.read().decode())
    except Exception as e:
        log.warning("Bitget GET %s failed: %s", path, e)
        return None

def bitget_account():
    """Bitget 계좌 정보 조회"""
    return bitget_get("/api/v2/mix/account/account?symbol=BTCUSDT&productType=USDT-FUTURES&marginCoin=USDT")

def bitget_positions():
    """Bitget 오픈 포지션 조회"""
    return bitget_get("/api/v2/mix/position/all-position?productType=USDT-FUTURES&marginCoin=USDT")

def bitget_close(symbol: str, hold_side: str):
    keys = load_exchange_keys()
    if not keys:
        return {"error": "No API keys"}
    ts = str(int(time.time() * 1000))
    path = "/api/v2/mix/order/close-positions"
    body = json.dumps({"symbol": symbol, "productType": "USDT-FUTURES", "holdSide": hold_side})
    msg = ts + "POST" + path + body
    sig = base64.b64encode(
        hmac.new(keys["api_secret"].encode(), msg.encode(), hashlib.sha256).digest()
    ).decode()
    headers = {
        "ACCESS-KEY": keys["api_key"],
        "ACCESS-SIGN": sig,
        "ACCESS-TIMESTAMP": ts,
        "ACCESS-PASSPHRASE": keys["passphrase"],
        "Content-Type": "application/json",
        "locale": "en-US",
    }
    req = Request("https://api.bitget.com" + path, data=body.encode(), headers=headers)
    try:
        resp = urlopen(req, timeout=10)
        return json.loads(resp.read().decode())
    except HTTPError as e:
        return json.loads(e.read().decode())
    except Exception as e:
        return {"error": str(e)}

# ---------------------------------------------------------------------------
# Bot command handlers
# ---------------------------------------------------------------------------
def cmd_help(chat_id, msg_id):
    text = (
        "/s - \uacc4\uc88c\uc0c1\ud0dc\n"
        "/p - \ud3ec\uc9c0\uc158\n"
        "/risk - \ub9ac\uc2a4\ud06c\n"
        "/scores - \ud2f0\uc5b4\n"
        "/t - \uac70\ub798\uae30\ub85d\n"
        "/close BTC - \uccad\uc0b0\n"
        "/closeall - \uc804\uccb4\uccad\uc0b0\n"
        "/restart - \uc7ac\uc2dc\uc791\n"
        "/l - \ub85c\uadf8"
    )
    send_telegram(text, chat_id, msg_id)

def cmd_status(chat_id, msg_id):
    # Bitget 실계좌 데이터
    acct = bitget_account()
    pos_data = bitget_positions()
    # 내부 통계 (승률 등)
    stats = api_get("/api/stats")

    if not acct or acct.get("code") != "00000":
        send_telegram("\u274c Bitget API \uc5f0\uacb0 \uc2e4\ud328", chat_id, msg_id)
        return

    d = acct.get("data", {})
    equity = float(d.get("accountEquity", 0))
    available = float(d.get("available", 0))
    upl = float(d.get("unrealizedPL", 0))
    margin = float(d.get("locked", 0))

    # 포지션 수
    positions = []
    if pos_data and pos_data.get("code") == "00000":
        for item in pos_data.get("data", []):
            qty = float(item.get("total", 0))
            if qty > 0:
                positions.append(item)
    pos_count = len(positions)

    # 내부 통계
    wr = 0
    w = 0
    l = 0
    if stats:
        wr = stats.get("win_rate", 0)
        w = stats.get("wins", 0)
        l = stats.get("losses", 0)

    margin_pct = (margin / equity * 100) if equity > 0 else 0
    upl_emoji = "\U0001f7e2" if upl >= 0 else "\U0001f534"

    text = (
        f"<b>\uacc4\uc88c</b> (Bitget)\n"
        f"\uc790\uc0b0: ${equity:.2f} | \uac00\uc6a9: ${available:.2f}\n"
        f"{upl_emoji} \ubbf8\uc2e4\ud604PnL: {upl:+.4f} USDT\n"
        f"\ub9c8\uc9c4: ${margin:.2f} ({margin_pct:.1f}%)\n"
        f"\ud3ec\uc9c0\uc158: {pos_count}\uac1c | \uc2b9\ub960: {wr:.1f}% ({w}W/{l}L)"
    )
    send_telegram(text, chat_id, msg_id)

def cmd_positions(chat_id, msg_id):
    pos_data = bitget_positions()
    if not pos_data or pos_data.get("code") != "00000":
        send_telegram("\u274c Bitget API \uc5f0\uacb0 \uc2e4\ud328", chat_id, msg_id)
        return

    positions = [p for p in pos_data.get("data", []) if float(p.get("total", 0)) > 0]
    if not positions:
        send_telegram("\ud3ec\uc9c0\uc158 \uc5c6\uc74c", chat_id, msg_id)
        return

    total_upl = sum(float(p.get("unrealizedPL", 0)) for p in positions)
    upl_emoji = "\U0001f7e2" if total_upl >= 0 else "\U0001f534"
    lines = [f"<b>\ud3ec\uc9c0\uc158</b> ({len(positions)}\uac1c) {upl_emoji} {total_upl:+.4f}\n"]

    for p in positions[:25]:
        side = p.get("holdSide", "?")
        s = "\U0001f7e2" if side == "long" else "\U0001f534"
        sym = p.get("symbol", "?").replace("USDT", "")
        lev = p.get("leverage", "?")
        upl = float(p.get("unrealizedPL", 0))
        entry = p.get("openPriceAvg", "?")
        mark = p.get("markPrice", "?")
        lines.append(f"{s} {sym} {lev}x {upl:+.4f}")

    text = "\n".join(lines)
    if len(text) > 4000:
        text = text[:4000] + "\n..."
    send_telegram(text, chat_id, msg_id)

def cmd_risk(chat_id, msg_id):
    risk = api_get("/api/risk/status")
    if not risk:
        send_telegram("\u274c API \uc5f0\uacb0 \uc2e4\ud328", chat_id, msg_id)
        return
    p = risk.get("portfolio", {})
    c = risk.get("check_stats", {})

    pos_tf = p.get("positions_by_tf", {})
    tf_info = " | ".join(f"{tf}m:{cnt}" for tf, cnt in sorted(pos_tf.items()) if cnt > 0)

    text = (
        f"<b>\ub9ac\uc2a4\ud06c</b>\n"
        f"DD: {p.get('current_drawdown_pct',0):.2f}% | \ub9c8\uc9c4: {p.get('margin_used_pct',0):.1f}%\n"
        f"\ub178\ucd9c: ${p.get('total_notional',0):,.0f} | \uc0c1\uad00: {p.get('correlated_risk_pct',0)}%\n"
        f"TF: {tf_info or '-'}\n"
        f"\uccb4\ud06c: {c.get('passed',0)}/{c.get('total_checks',0)} | "
        f"\uc11c\ud0b7\ube0c: {'ON' if p.get('circuit_breaker_active') else 'OFF'}"
    )
    send_telegram(text, chat_id, msg_id)

def cmd_scores(chat_id, msg_id):
    data = api_get("/api/symbols/scores")
    if not data:
        send_telegram("\u274c API \uc5f0\uacb0 \uc2e4\ud328", chat_id, msg_id)
        return
    ranking = data.get("ranking", [])
    if not ranking:
        send_telegram("\ud2f0\uc5b4 \ub370\uc774\ud130 \uc5c6\uc74c (\ucd5c\uc18c 20\uac74 \ud544\uc694)", chat_id, msg_id)
        return

    ranking = [r for r in ranking if r.get("timeframe") not in ("1", "5")]

    lines = [f"<b>\ud2f0\uc5b4</b> (Top 10)\n"]
    for i, r in enumerate(ranking[:10], 1):
        t = r.get("tier", "?")
        sym = r.get("symbol", "?").replace("USDT", "")
        tf = r.get("timeframe", "?")
        sc = r.get("score", 0)
        lines.append(f"{i}. [{t}] {sym} {tf}m ({sc:.0f}\uc810)")

    tier_counts = {}
    for r in ranking:
        t = r.get("tier", "?")
        tier_counts[t] = tier_counts.get(t, 0) + 1
    summary = " ".join(f"{t}:{c}" for t, c in sorted(tier_counts.items()))
    lines.append(f"\n\ubd84\ud3ec: {summary}")

    send_telegram("\n".join(lines), chat_id, msg_id)

def cmd_trades(chat_id, msg_id):
    stats = api_get("/api/stats")
    if not stats:
        send_telegram("\u274c API \uc5f0\uacb0 \uc2e4\ud328", chat_id, msg_id)
        return

    trades = stats.get("recent_trades", [])
    if not trades:
        total = stats.get("total_trades", 0)
        send_telegram(f"\uac70\ub798\uae30\ub85d: \ucd1d {total}\uac74 (\uc0c1\uc138 \uc5c6\uc74c)", chat_id, msg_id)
        return

    lines = [f"<b>\ucd5c\uadfc\uac70\ub798</b> ({len(trades)}\uac74)\n"]
    for t in trades[:10]:
        sym = t.get("symbol", "?").replace("USDT", "")
        pnl = t.get("pnl", 0)
        mark = "\u2705" if pnl >= 0 else "\u274c"
        lines.append(f"{mark} {sym} {pnl:+.4f}")

    send_telegram("\n".join(lines), chat_id, msg_id)

def cmd_close(chat_id, msg_id, args: str):
    if not args:
        send_telegram("\u2753 \uc0ac\uc6a9\ubc95: /close BTCUSDT [long/short]", chat_id, msg_id)
        return

    parts = args.strip().upper().split()
    symbol = parts[0]
    if not symbol.endswith("USDT"):
        symbol += "USDT"

    # Find the position to determine side
    data = api_get("/api/positions")
    positions = data if isinstance(data, list) else (data or {}).get("positions", [])
    matches = [p for p in positions if p.get("symbol") == symbol]

    if not matches:
        send_telegram(f"\u274c {symbol} \ud3ec\uc9c0\uc158\uc744 \ucc3e\uc744 \uc218 \uc5c6\uc74c", chat_id, msg_id)
        return

    if len(parts) > 1:
        side = parts[1].lower()
        matches = [p for p in matches if p.get("side") == side]

    results = []
    for p in matches:
        side = p.get("side", "long")
        r = bitget_close(symbol, side)
        ok = r.get("code") == "00000" if r else False
        ok_emoji = "\u2705" if ok else "\u274c"
        status_msg = "OK" if ok else r.get("msg", "FAIL")
        results.append(f"{ok_emoji} {symbol} {side}: {status_msg}")
        time.sleep(0.3)

    send_telegram("\n".join(results), chat_id, msg_id)

def cmd_closeall(chat_id, msg_id):
    data = api_get("/api/positions")
    positions = data if isinstance(data, list) else (data or {}).get("positions", [])

    if not positions:
        send_telegram("\U0001f4cb \uccad\uc0b0\ud560 \ud3ec\uc9c0\uc158 \uc5c6\uc74c", chat_id, msg_id)
        return

    send_telegram(f"\U0001f4a5 {len(positions)}\uac1c \ud3ec\uc9c0\uc158 \uc804\uccb4 \uccad\uc0b0 \uc2dc\uc791...", chat_id, msg_id)

    ok_count = 0
    fail_count = 0
    for p in positions:
        sym = p.get("symbol")
        side = p.get("side", "long")
        r = bitget_close(sym, side)
        if r and r.get("code") == "00000":
            ok_count += 1
        else:
            fail_count += 1
        time.sleep(0.3)

    send_telegram(
        f"\U0001f4a5 \uccad\uc0b0 \uc644\ub8cc: \u2705 {ok_count}\uac1c \uc131\uacf5 / \u274c {fail_count}\uac1c \uc2e4\ud328",
        chat_id, msg_id
    )

def cmd_restart(chat_id, msg_id):
    send_telegram("\U0001f504 \uc11c\ubc84 \uc7ac\uc2dc\uc791 \uc911...", chat_id, msg_id)
    try:
        subprocess.run(
            ["sudo", "systemctl", "restart", SERVICE_NAME],
            timeout=15, capture_output=True
        )
        time.sleep(3)
        result = subprocess.run(
            ["systemctl", "is-active", SERVICE_NAME],
            capture_output=True, text=True, timeout=5
        )
        status = result.stdout.strip()
        if status == "active":
            send_telegram(f"\u2705 {SERVICE_NAME} \uc7ac\uc2dc\uc791 \uc131\uacf5", chat_id, msg_id)
        else:
            send_telegram(f"\u274c {SERVICE_NAME} \uc0c1\ud0dc: {status}", chat_id, msg_id)
    except Exception as e:
        send_telegram(f"\u274c \uc7ac\uc2dc\uc791 \uc2e4\ud328: {e}", chat_id, msg_id)

def cmd_logs(chat_id, msg_id):
    try:
        result = subprocess.run(
            ["journalctl", "-u", SERVICE_NAME, "-n", "15", "--no-pager", "-o", "short-iso"],
            capture_output=True, text=True, timeout=5
        )
        lines = result.stdout.strip().split("\n")
        formatted = []
        for line in lines[-15:]:
            m = re.match(r"^.*?tv-webhook-trader\[\d+\]:\s*(.+)$", line)
            msg = m.group(1) if m else line.strip()
            if len(msg) > 100:
                msg = msg[:100] + "..."
            formatted.append(f"<code>{msg}</code>")

        text = f"<b>\ub85c\uadf8</b> ({len(formatted)}\uc904)\n\n" + "\n".join(formatted)
        if len(text) > 4000:
            text = text[:4000] + "\n..."
        send_telegram(text, chat_id, msg_id)
    except Exception as e:
        send_telegram(f"\u274c \ub85c\uadf8 \uc870\ud68c \uc2e4\ud328: {e}", chat_id, msg_id)

# ---------------------------------------------------------------------------
# Command router
# ---------------------------------------------------------------------------
COMMANDS = {
    "/help": cmd_help,
    "/start": cmd_help,
    "/status": cmd_status,
    "/s": cmd_status,
    "/positions": cmd_positions,
    "/pos": cmd_positions,
    "/p": cmd_positions,
    "/risk": cmd_risk,
    "/scores": cmd_scores,
    "/trades": cmd_trades,
    "/t": cmd_trades,
    "/logs": cmd_logs,
    "/l": cmd_logs,
    "/restart": cmd_restart,
}

def handle_command(text: str, chat_id: str, msg_id: int):
    text = text.strip()
    # Handle /command@botname format
    if "@" in text.split()[0]:
        text = text.split("@")[0] + " " + " ".join(text.split()[1:])
        text = text.strip()

    parts = text.split(maxsplit=1)
    cmd = parts[0].lower()
    args = parts[1] if len(parts) > 1 else ""

    if cmd in ("/close",):
        cmd_close(chat_id, msg_id, args)
    elif cmd in ("/closeall",):
        cmd_closeall(chat_id, msg_id)
    elif cmd in COMMANDS:
        COMMANDS[cmd](chat_id, msg_id)
    # Ignore unknown commands silently

# ---------------------------------------------------------------------------
# Telegram polling (for interactive commands)
# ---------------------------------------------------------------------------
def poll_updates():
    """Long-poll for incoming Telegram messages."""
    offset = 0
    log.info("Starting Telegram command polling...")
    while _running:
        try:
            result = tg_api("getUpdates", {
                "offset": offset,
                "timeout": 30,
                "allowed_updates": ["message"],
            }, timeout=35)

            if not result or not result.get("ok"):
                time.sleep(POLL_INTERVAL)
                continue

            for update in result.get("result", []):
                offset = update["update_id"] + 1
                msg = update.get("message", {})
                text = msg.get("text", "")
                chat_id = str(msg.get("chat", {}).get("id", ""))
                msg_id = msg.get("message_id")

                # Security: only respond to authorized chat
                if chat_id != CHAT_ID:
                    log.warning("Unauthorized chat_id: %s", chat_id)
                    continue

                if text.startswith("/"):
                    log.info("Command: %s from %s", text, chat_id)
                    try:
                        handle_command(text, chat_id, msg_id)
                    except Exception as e:
                        log.error("Command error: %s", e)
                        send_telegram(f"\u274c \uc624\ub958: {e}", chat_id, msg_id)

        except Exception as e:
            log.warning("Poll error: %s", e)
            time.sleep(POLL_INTERVAL)

# ---------------------------------------------------------------------------
# Line Batcher
# ---------------------------------------------------------------------------
class LineBatcher:
    def __init__(self, flush_fn, delay: float = BATCH_DELAY):
        self._flush_fn = flush_fn
        self._delay = delay
        self._buffer: list = []
        self._lock = threading.Lock()
        self._timer = None

    def add(self, line: str):
        with self._lock:
            self._buffer.append(line)
            if self._timer is None:
                self._timer = threading.Timer(self._delay, self._flush)
                self._timer.daemon = True
                self._timer.start()

    def _flush(self):
        with self._lock:
            lines = self._buffer[:]
            self._buffer.clear()
            self._timer = None
        if lines:
            self._flush_fn(lines)

    def force_flush(self):
        if self._timer:
            self._timer.cancel()
        self._flush()

# ---------------------------------------------------------------------------
# Journal follower
# ---------------------------------------------------------------------------
_running = True

def follow_journal():
    while _running:
        log.info("Starting journalctl for %s", SERVICE_NAME)
        try:
            proc = subprocess.Popen(
                ["journalctl", "-u", SERVICE_NAME, "-f", "-n", "0", "-o", "short-iso"],
                stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                text=True, bufsize=1,
            )
            for line in proc.stdout:
                if not _running:
                    proc.terminate()
                    return
                yield line
            proc.wait()
            log.warning("journalctl exited (%s), reconnecting...", proc.returncode)
        except FileNotFoundError:
            log.error("journalctl not found")
            time.sleep(RECONNECT_DELAY * 6)
        except Exception as e:
            log.error("journalctl error: %s", e)
        if _running:
            time.sleep(RECONNECT_DELAY)

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    global _running

    if not BOT_TOKEN or not CHAT_ID:
        log.error("TELEGRAM_BOT_TOKEN / TELEGRAM_CHAT_ID required!")
        sys.exit(1)

    def flush_batch(lines):
        body = "\n".join(lines)
        if len(body) > 4000:
            chunks = [lines[i:i+10] for i in range(0, len(lines), 10)]
            for chunk in chunks:
                send_telegram("\n".join(chunk))
        else:
            send_telegram(body)

    batcher = LineBatcher(flush_batch)

    def shutdown(signum, frame):
        global _running
        log.info("Shutting down (signal %s)", signum)
        _running = False
        batcher.force_flush()
        sys.exit(0)

    signal.signal(signal.SIGTERM, shutdown)
    signal.signal(signal.SIGINT, shutdown)

    # Start command polling thread
    poll_thread = threading.Thread(target=poll_updates, daemon=True)
    poll_thread.start()
    log.info("Command polling thread started")

    # Startup notification
    now_kst = datetime.now(KST).strftime("%m/%d %H:%M")
    send_telegram(f"Bot \uc2dc\uc791 ({now_kst} KST) - /help")

    # Follow journal for alerts
    for raw_line in follow_journal():
        raw_line = raw_line.rstrip("\n")
        if not raw_line:
            continue
        if not is_important(raw_line):
            continue
        formatted = format_line(raw_line)
        log.info(">> %s", formatted)
        batcher.add(formatted)

if __name__ == "__main__":
    main()

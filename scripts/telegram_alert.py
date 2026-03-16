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
# Patterns to match
# ---------------------------------------------------------------------------
IMPORTANT_PATTERNS = [
    re.compile(r"\[SFX\]",        re.IGNORECASE),
    re.compile(r"\bENTRY\b"),
    re.compile(r"\bTP\d?\b"),
    re.compile(r"\bSL\b"),
    re.compile(r"\bOK\b.*sz="),
    re.compile(r"\bSKIP\b"),
    re.compile(r"\bRISK\b"),
    re.compile(r"\bFAIL\b"),
    re.compile(r"\bSHADOW\b"),
    re.compile(r"\bORDER\b",      re.IGNORECASE),
    re.compile(r"\bFILLED\b",     re.IGNORECASE),
    re.compile(r"\berror\b",      re.IGNORECASE),
    re.compile(r"CIRCUIT.?BREAKER", re.IGNORECASE),
    re.compile(r"TPSL fail"),
    re.compile(r"\[QUEUE\] Full"),
    re.compile(r"CLOSE"),
    re.compile(r"ReEntry",        re.IGNORECASE),
]

def is_important(line: str) -> bool:
    return any(p.search(line) for p in IMPORTANT_PATTERNS)

# ---------------------------------------------------------------------------
# Classify & decorate
# ---------------------------------------------------------------------------
def classify(line: str) -> str:
    lo = line.upper()
    if "FAIL" in lo or "ERROR" in lo or "TPSL FAIL" in lo:
        return "\u26a0\ufe0f"
    if "CIRCUIT" in lo and "BREAKER" in lo:
        return "\U0001f6a8"
    if "SKIP" in lo or "RISK" in lo:
        return "\u26d4"
    if "CLOSE" in lo:
        return "\U0001f4a5"
    if " SL " in lo:
        return "\U0001f534"
    if "TP" in lo and any(c.isdigit() for c in lo[lo.index("TP"):lo.index("TP")+4]):
        return "\U0001f3af"
    if "ENTRY" in lo or "OK" in lo or "SHADOW" in lo:
        if "BUY" in lo or "LONG" in lo:
            return "\U0001f7e2"
        if "SELL" in lo or "SHORT" in lo:
            return "\U0001f534"
        return "\U0001f4e8"
    if "REENTRY" in lo:
        return "\U0001f504"
    if "[SFX]" in line:
        if "buy" in line.lower():
            return "\U0001f7e2"
        if "sell" in line.lower():
            return "\U0001f534"
        return "\U0001f4e8"
    if "ORDER" in lo or "FILLED" in lo:
        return "\U0001f4b0"
    if "QUEUE" in lo and "FULL" in lo:
        return "\u26a0\ufe0f"
    return "\U0001f4ac"

def format_line(line: str) -> str:
    m = re.match(r"^.*?tv-webhook-trader\[\d+\]:\s*(.+)$", line)
    msg = m.group(1) if m else line.strip()
    emoji = classify(msg)
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
def api_get(path: str):
    try:
        url = f"{DASHBOARD_URL}{path}"
        req = Request(url, headers={"Accept": "application/json"})
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
        with open(cfg.get("api_keys_path", "")) as f:
            return json.load(f)
    except Exception as e:
        log.error("Failed to load exchange keys: %s", e)
        return None

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
        "\U0001f916 <b>HFT Trader Bot Commands</b>\n\n"
        "/status - \U0001f4ca \uacc4\uc88c \uc0c1\ud0dc (\uc794\uace0, PnL, \ud3ec\uc9c0\uc158)\n"
        "/positions - \U0001f4cb \uc624\ud508 \ud3ec\uc9c0\uc158 \ubaa9\ub85d\n"
        "/risk - \u26a1 \ub9ac\uc2a4\ud06c \uc0c1\ud0dc\n"
        "/scores - \U0001f3c6 \uc2ec\ubcfc \ud2f0\uc5b4 \ub7ad\ud0b9\n"
        "/trades - \U0001f4c8 \ucd5c\uadfc \uac70\ub798 \uae30\ub85d\n"
        "/close <symbol> - \U0001f4a5 \ud3ec\uc9c0\uc158 \uccad\uc0b0\n"
        "/closeall - \U0001f4a5 \uc804\uccb4 \uccad\uc0b0\n"
        "/restart - \U0001f504 \uc11c\ubc84 \uc7ac\uc2dc\uc791\n"
        "/logs - \U0001f4dc \ucd5c\uadfc \ub85c\uadf8\n"
        "/help - \u2753 \ub3c4\uc6c0\ub9d0"
    )
    send_telegram(text, chat_id, msg_id)

def cmd_status(chat_id, msg_id):
    stats = api_get("/api/stats")
    risk = api_get("/api/risk/status")
    if not stats:
        send_telegram("\u274c API \uc5f0\uacb0 \uc2e4\ud328", chat_id, msg_id)
        return

    portfolio = (risk or {}).get("portfolio", {})
    now_kst = datetime.now(KST).strftime("%m/%d %H:%M")
    bal = stats.get("balance", 0)
    pnl = stats.get("total_pnl", 0)
    roi = stats.get("roi_pct", 0)
    wr = stats.get("win_rate", 0)
    wins = stats.get("wins", 0)
    losses = stats.get("losses", 0)
    total_trades = stats.get("total_trades", 0)
    opos = stats.get("open_positions", 0)
    peak = stats.get("peak_balance", bal)
    dd = portfolio.get("current_drawdown_pct", 0)
    margin_pct = portfolio.get("margin_used_pct", 0)
    daily = portfolio.get("daily_pnl", 0)
    weekly = portfolio.get("weekly_pnl", 0)
    cb = portfolio.get("circuit_breaker_active", False)

    pnl_emoji = "\U0001f7e2" if pnl >= 0 else "\U0001f534"
    daily_emoji = "\U0001f7e2" if daily >= 0 else "\U0001f534"

    text = (
        f"\U0001f4ca <b>HFT Trader Status</b>  <i>{now_kst} KST</i>\n"
        f"\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\n"
        f"\U0001f4b0 \uc794\uace0: <b>${bal:.2f}</b> (Peak: ${peak:.2f})\n"
        f"{pnl_emoji} \ucd1d PnL: <b>{pnl:+.4f}</b> ({roi:+.2f}%)\n"
        f"{daily_emoji} \uc77c\uac04: {daily:+.4f} | \uc8fc\uac04: {weekly:+.4f}\n"
        f"\U0001f4c9 DD: {dd:.2f}% | \ub9c8\uc9c4: {margin_pct:.1f}%\n"
        f"\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\u2500\n"
        f"\U0001f4cb \ud3ec\uc9c0\uc158: <b>{opos}</b>\uac1c\n"
        f"\U0001f3af \uc2b9\ub960: {wr:.1f}% ({wins}W/{losses}L, \ucd1d{total_trades})\n"
        f"\U0001f6a8 \uc11c\ud0b7\ube0c\ub808\uc774\ucee4: " + ("ON \u26a0\ufe0f" if cb else "OFF \u2705")
    )
    send_telegram(text, chat_id, msg_id)

def cmd_positions(chat_id, msg_id):
    data = api_get("/api/positions")
    if not data:
        send_telegram("\u274c API \uc5f0\uacb0 \uc2e4\ud328", chat_id, msg_id)
        return

    positions = data if isinstance(data, list) else data.get("positions", [])
    if not positions:
        send_telegram("\U0001f4cb \uc624\ud508 \ud3ec\uc9c0\uc158 \uc5c6\uc74c", chat_id, msg_id)
        return

    # Group by TF
    by_tf = {}
    for p in positions:
        tf = p.get("timeframe", "?")
        by_tf.setdefault(tf, []).append(p)

    lines = [f"\U0001f4cb <b>\uc624\ud508 \ud3ec\uc9c0\uc158</b> ({len(positions)}\uac1c)\n"]
    tf_order = ["15", "30", "60", "ext"]
    for tf in tf_order:
        plist = by_tf.pop(tf, [])
        if not plist:
            continue
        lines.append(f"\n<b>[{tf}m]</b> {len(plist)}\uac1c")
        for p in plist[:10]:
            side_e = "\U0001f7e2" if p.get("side") == "long" else "\U0001f534"
            sym = p.get("symbol", "?").replace("USDT", "")
            lev = p.get("leverage", "?")
            entry = p.get("entry_price", 0)
            lines.append(f"  {side_e} {sym} {lev}x @ {entry}")
    # Remaining TFs
    for tf, plist in by_tf.items():
        lines.append(f"\n<b>[{tf}]</b> {len(plist)}\uac1c")
        for p in plist[:5]:
            side_e = "\U0001f7e2" if p.get("side") == "long" else "\U0001f534"
            sym = p.get("symbol", "?").replace("USDT", "")
            lines.append(f"  {side_e} {sym} {p.get('leverage','?')}x")

    text = "\n".join(lines)
    if len(text) > 4000:
        text = text[:4000] + "\n..."
    send_telegram(text, chat_id, msg_id)

def cmd_risk(chat_id, msg_id):
    risk = api_get("/api/risk/status")
    if not risk:
        send_telegram("\u274c API \uc5f0\uacb0 \uc2e4\ud328", chat_id, msg_id)
        return
    portfolio = risk.get("portfolio", {})
    checks = risk.get("check_stats", {})

    pos_tf = portfolio.get("positions_by_tf", {})
    exp_tf = portfolio.get("exposure_by_tf", {})
    tf_lines = []
    for tf in ["15", "30", "60"]:
        cnt = pos_tf.get(tf, 0)
        exp = exp_tf.get(tf, 0)
        if cnt > 0:
            tf_lines.append(f"  {tf}m: {cnt}\uac1c (${exp:.0f})")

    text = (
        f"\u26a1 <b>\ub9ac\uc2a4\ud06c \uc0c1\ud0dc</b>\n\n"
        f"\U0001f6e1 \ub4dc\ub85c\ub2e4\uc6b4: {portfolio.get('current_drawdown_pct',0):.2f}%\n"
        f"\U0001f4b3 \ub9c8\uc9c4 \uc0ac\uc6a9: {portfolio.get('margin_used_pct',0):.1f}%\n"
        f"\U0001f4b5 \ucd1d \ub178\ucd9c: ${portfolio.get('total_notional',0):,.0f}\n"
        f"\U0001f517 \uc0c1\uad00 \ub9ac\uc2a4\ud06c: {portfolio.get('correlated_risk_pct',0)}%\n"
        f"\U0001f6a8 \uc11c\ud0b7\ube0c: {'ON' if portfolio.get('circuit_breaker_active') else 'OFF'}\n"
        f"\n<b>TF\ubcc4 \ud3ec\uc9c0\uc158:</b>\n"
        + "\n".join(tf_lines) + "\n"
        f"\n\u2705 \ub9ac\uc2a4\ud06c \uccb4\ud06c: {checks.get('passed',0)}/{checks.get('total_checks',0)} pass"
    )
    send_telegram(text, chat_id, msg_id)

def cmd_scores(chat_id, msg_id):
    data = api_get("/api/symbols/scores")
    if not data:
        send_telegram("\u274c API \uc5f0\uacb0 \uc2e4\ud328", chat_id, msg_id)
        return
    ranking = data.get("ranking", [])
    if not ranking:
        send_telegram("\U0001f3c6 \uc2a4\ucf54\uc5b4\ub9c1 \ub370\uc774\ud130 \uc5c6\uc74c (\ucd5c\uc18c 20\uac74 \ud544\uc694)", chat_id, msg_id)
        return

    # Filter 1m/5m
    ranking = [r for r in ranking if r.get("timeframe") not in ("1", "5")]

    tier_emoji = {"S": "\U0001f451", "A": "\U0001f947", "B": "\U0001f948", "C": "\U0001f949", "D": "\u26aa", "X": "\u274c"}
    lines = [f"\U0001f3c6 <b>\uc2ec\ubcfc \ud2f0\uc5b4 \ub7ad\ud0b9</b> (Top 15)\n"]
    for i, r in enumerate(ranking[:15], 1):
        t = r.get("tier", "?")
        em = tier_emoji.get(t, "\u2753")
        sym = r.get("symbol", "?").replace("USDT", "")
        tf = r.get("timeframe", "?")
        sc = r.get("score", 0)
        wr = r.get("win_rate", 0)
        trades = r.get("total_trades", 0)
        lines.append(f"{i}. {em}{t} <b>{sym}</b>:{tf}m  \uc810\uc218:{sc:.0f} WR:{wr:.0f}% ({trades})")

    # Tier summary
    tier_counts = {}
    for r in ranking:
        t = r.get("tier", "?")
        tier_counts[t] = tier_counts.get(t, 0) + 1
    summary = " | ".join(f"{t}:{c}" for t, c in sorted(tier_counts.items()))
    lines.append(f"\n\ud2f0\uc5b4 \ubd84\ud3ec: {summary}")

    send_telegram("\n".join(lines), chat_id, msg_id)

def cmd_trades(chat_id, msg_id):
    stats = api_get("/api/stats")
    if not stats:
        send_telegram("\u274c API \uc5f0\uacb0 \uc2e4\ud328", chat_id, msg_id)
        return

    trades = stats.get("recent_trades", [])
    if not trades:
        total = stats.get("total_trades", 0)
        if total == 0:
            send_telegram("\U0001f4c8 \uac70\ub798 \uae30\ub85d \uc5c6\uc74c (\ub9ac\uc14b \ud6c4 \uc0c8\ub85c \uc218\uc9d1 \uc911)", chat_id, msg_id)
        else:
            send_telegram(f"\U0001f4c8 \ucd1d {total}\uac74 \uac70\ub798 \uc644\ub8cc (API\uc5d0\uc11c \uc0c1\uc138 \uc870\ud68c \ubd88\uac00)", chat_id, msg_id)
        return

    lines = [f"\U0001f4c8 <b>\ucd5c\uadfc \uac70\ub798</b> ({len(trades)}\uac74)\n"]
    for t in trades[:10]:
        sym = t.get("symbol", "?").replace("USDT", "")
        side = t.get("side", "?")
        pnl = t.get("pnl", 0)
        side_e = "\U0001f7e2" if side == "long" else "\U0001f534"
        pnl_e = "\u2705" if pnl >= 0 else "\u274c"
        lines.append(f"{side_e} {sym} {side} {pnl_e} {pnl:+.4f} USDT")

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

        text = f"\U0001f4dc <b>\ucd5c\uadfc \ub85c\uadf8</b> ({len(formatted)}\uc904)\n\n" + "\n".join(formatted)
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
        header = f"<b>\U0001f4e1 {SERVICE_NAME}</b>"
        body = "\n".join(lines)
        msg = f"{header}\n\n{body}"
        if len(msg) > 4000:
            chunks = [lines[i:i+10] for i in range(0, len(lines), 10)]
            for chunk in chunks:
                send_telegram(f"{header}\n\n" + "\n".join(chunk))
        else:
            send_telegram(msg)

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
    now_kst = datetime.now(KST).strftime("%Y-%m-%d %H:%M:%S")
    send_telegram(
        f"\U0001f680 <b>{SERVICE_NAME}</b> Bot \uc2dc\uc791\n"
        f"\U0001f552 {now_kst} KST\n"
        f"\U0001f4f1 /help \ub85c \uba85\ub839\uc5b4 \ud655\uc778"
    )

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

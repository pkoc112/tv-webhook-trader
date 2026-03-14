#!/usr/bin/env python3
"""
Telegram Alert Service for tv-webhook-trader
Monitors journalctl logs and sends formatted notifications to Telegram.

Environment variables:
    TELEGRAM_BOT_TOKEN  - Bot token from @BotFather
    TELEGRAM_CHAT_ID    - Chat/group ID for notifications
"""

import os
import re
import sys
import time
import signal
import subprocess
import threading
import logging
from collections import deque
from urllib.request import Request, urlopen
from urllib.error import URLError
from urllib.parse import quote
import json

# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------
BOT_TOKEN = os.environ.get("TELEGRAM_BOT_TOKEN", "")
CHAT_ID = os.environ.get("TELEGRAM_CHAT_ID", "")
SERVICE_NAME = os.environ.get("ALERT_SERVICE_NAME", "tv-webhook-trader")

RATE_LIMIT_MAX = 20          # max messages per window
RATE_LIMIT_WINDOW = 60       # seconds
BATCH_DELAY = 2.0            # seconds to wait before flushing batched lines
RECONNECT_DELAY = 5          # seconds before restarting journalctl

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [telegram-alert] %(levelname)s %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S",
)
log = logging.getLogger("telegram-alert")

# ---------------------------------------------------------------------------
# Patterns to match (case-insensitive where noted)
# ---------------------------------------------------------------------------
IMPORTANT_PATTERNS = [
    re.compile(r"\[SFX\]",        re.IGNORECASE),
    re.compile(r"\bENTRY\b"),
    re.compile(r"\bTP\b"),
    re.compile(r"\bSL\b"),
    re.compile(r"\bOK\b.*sz="),
    re.compile(r"\bSKIP\b"),
    re.compile(r"\bRISK\b"),
    re.compile(r"\bFAIL\b"),
    re.compile(r"\bSHADOW\b"),
    re.compile(r"\bORDER\b",      re.IGNORECASE),
    re.compile(r"\bFILLED\b",     re.IGNORECASE),
    re.compile(r"\berror\b",      re.IGNORECASE),
    re.compile(r"CIRCUIT BREAKER"),
    re.compile(r"TPSL fail"),
    re.compile(r"\[QUEUE\] Full"),
]

def is_important(line: str) -> bool:
    return any(p.search(line) for p in IMPORTANT_PATTERNS)

# ---------------------------------------------------------------------------
# Classify & decorate
# ---------------------------------------------------------------------------
def classify(line: str) -> str:
    """Return an emoji prefix based on the log line content."""
    lo = line.upper()

    # Errors first
    if "FAIL" in lo or "ERROR" in lo or "TPSL FAIL" in lo:
        return "\u26a0\ufe0f"          # warning sign
    if "CIRCUIT BREAKER" in lo:
        return "\U0001f6a8"            # rotating light
    if "SKIP" in lo or "RISK" in lo:
        return "\u26d4"                # no entry sign
    if " SL " in lo:
        return "\u26d4"                # stop loss
    if " TP " in lo:
        return "\U0001f3af"            # target / TP hit

    # Entry / order
    if "ENTRY" in lo or "OK" in lo or "SHADOW" in lo:
        if "BUY" in lo:
            return "\U0001f7e2"        # green circle
        if "SELL" in lo:
            return "\U0001f534"        # red circle
        return "\U0001f4e8"            # incoming envelope

    if "[SFX]" in line:
        if "buy" in line.lower():
            return "\U0001f7e2"
        if "sell" in line.lower():
            return "\U0001f534"
        return "\U0001f4e8"

    if "ORDER" in lo or "FILLED" in lo:
        return "\U0001f4b0"            # money bag
    if "QUEUE" in lo and "FULL" in lo:
        return "\u26a0\ufe0f"

    return "\U0001f4ac"                # speech balloon (generic)

def format_line(line: str) -> str:
    """Strip journalctl metadata prefix, keep the spdlog message."""
    # journalctl -o short-iso produces lines like:
    # 2025-06-01T12:00:00+0000 hostname tv-webhook-trader[1234]: <msg>
    # We want just <msg>.
    m = re.match(r"^.*?tv-webhook-trader\[\d+\]:\s*(.+)$", line)
    msg = m.group(1) if m else line.strip()
    emoji = classify(msg)
    return f"{emoji} {msg}"

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
        """Return True if we can send, False if rate-limited."""
        now = time.monotonic()
        with self._lock:
            # Evict old timestamps
            while self._timestamps and self._timestamps[0] < now - self._window:
                self._timestamps.popleft()
            if len(self._timestamps) >= self._max:
                return False
            self._timestamps.append(now)
            return True

    def wait(self):
        """Block until a slot is available."""
        while not self.acquire():
            time.sleep(1)

# ---------------------------------------------------------------------------
# Telegram sender
# ---------------------------------------------------------------------------
rate_limiter = RateLimiter(RATE_LIMIT_MAX, RATE_LIMIT_WINDOW)

def send_telegram(text: str):
    """Send a message via Telegram Bot API with rate limiting."""
    if not BOT_TOKEN or not CHAT_ID:
        log.warning("TELEGRAM_BOT_TOKEN or TELEGRAM_CHAT_ID not set, printing instead")
        print(text, flush=True)
        return

    rate_limiter.wait()

    url = f"https://api.telegram.org/bot{BOT_TOKEN}/sendMessage"
    payload = json.dumps({
        "chat_id": CHAT_ID,
        "text": text,
        "parse_mode": "HTML",
        "disable_web_page_preview": True,
    }).encode("utf-8")

    req = Request(url, data=payload, headers={"Content-Type": "application/json"})
    for attempt in range(3):
        try:
            resp = urlopen(req, timeout=10)
            if resp.status == 200:
                return
            log.warning("Telegram API returned %d", resp.status)
        except URLError as e:
            log.warning("Telegram send failed (attempt %d): %s", attempt + 1, e)
            time.sleep(2 ** attempt)
    log.error("Failed to send Telegram message after 3 attempts")

# ---------------------------------------------------------------------------
# Batcher: accumulate lines for BATCH_DELAY seconds then flush
# ---------------------------------------------------------------------------
class LineBatcher:
    def __init__(self, flush_fn, delay: float = BATCH_DELAY):
        self._flush_fn = flush_fn
        self._delay = delay
        self._buffer: list[str] = []
        self._lock = threading.Lock()
        self._timer: threading.Timer | None = None

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
    """Spawn journalctl -f and yield lines forever, reconnecting on failure."""
    while _running:
        log.info("Starting journalctl for %s", SERVICE_NAME)
        try:
            proc = subprocess.Popen(
                [
                    "journalctl",
                    "-u", SERVICE_NAME,
                    "-f",                   # follow
                    "-n", "0",              # no historical lines
                    "-o", "short-iso",      # consistent timestamp format
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                text=True,
                bufsize=1,
            )
            for line in proc.stdout:
                if not _running:
                    proc.terminate()
                    return
                yield line
            proc.wait()
            log.warning("journalctl exited with code %s, reconnecting...", proc.returncode)
        except FileNotFoundError:
            log.error("journalctl not found - is systemd available?")
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
        log.warning("TELEGRAM_BOT_TOKEN / TELEGRAM_CHAT_ID not set. "
                     "Messages will be printed to stdout instead.")

    def flush_batch(lines: list[str]):
        header = f"<b>{SERVICE_NAME}</b>"
        body = "\n".join(lines)
        # Telegram message limit is 4096 chars
        msg = f"{header}\n\n{body}"
        if len(msg) > 4000:
            # Split into chunks
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

    # Startup notification
    send_telegram(f"\U0001f680 <b>{SERVICE_NAME}</b> Telegram alerts started")

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

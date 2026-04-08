#!/usr/bin/env python3
"""
Daily Trade Quality Report for tv-webhook-trader
- Reads shadow/live trade data from dashboard API
- Calculates key metrics from the improvement report recommendations
- Sends structured Telegram report
- Tracks trends over time for automated post-freeze optimization

Runs daily via systemd timer or cron.
"""

import os
import sys
import json
import logging
from datetime import datetime, timezone, timedelta
from urllib.request import Request, urlopen
from urllib.error import URLError, HTTPError
from pathlib import Path

# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------
BOT_TOKEN = os.environ.get("TELEGRAM_BOT_TOKEN", "")
CHAT_ID = os.environ.get("TELEGRAM_CHAT_ID", "")
DASHBOARD_URL = os.environ.get("DASHBOARD_URL", "http://127.0.0.1:5000")
DATA_DIR = Path(os.environ.get("DATA_DIR", "/home/ubuntu/tv-webhook-trader/data"))
REPORT_HISTORY = DATA_DIR / "quality_report_history.json"

KST = timezone(timedelta(hours=9))

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [quality-report] %(levelname)s %(message)s",
    datefmt="%Y-%m-%d %H:%M:%S",
)
log = logging.getLogger("quality-report")


def send_telegram(text: str):
    """Send message via Telegram Bot API."""
    if not BOT_TOKEN or not CHAT_ID:
        log.error("Missing TELEGRAM_BOT_TOKEN or TELEGRAM_CHAT_ID")
        return
    url = f"https://api.telegram.org/bot{BOT_TOKEN}/sendMessage"
    payload = json.dumps({
        "chat_id": CHAT_ID,
        "text": text,
        "parse_mode": "HTML",
        "disable_web_page_preview": True,
    }).encode("utf-8")
    req = Request(url, data=payload, headers={"Content-Type": "application/json"})
    try:
        with urlopen(req, timeout=15) as resp:
            resp.read()
        log.info("Telegram message sent")
    except Exception as e:
        log.error(f"Telegram send failed: {e}")


def api_get(endpoint: str) -> dict:
    """GET from dashboard API."""
    url = f"{DASHBOARD_URL}{endpoint}"
    try:
        req = Request(url, headers={"Accept": "application/json"})
        with urlopen(req, timeout=10) as resp:
            return json.loads(resp.read().decode("utf-8"))
    except Exception as e:
        log.error(f"API call failed: {endpoint} -> {e}")
        return {}


def load_state_file(name: str) -> dict:
    """Load a JSON state file from data directory."""
    path = DATA_DIR / name
    if not path.exists():
        return {}
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception as e:
        log.error(f"Failed to load {name}: {e}")
        return {}


def analyze_trades(trades: list) -> dict:
    """Analyze trade list for quality metrics."""
    if not trades:
        return {"count": 0}

    wins = [t for t in trades if t.get("pnl", 0) > 0]
    losses = [t for t in trades if t.get("pnl", 0) <= 0]

    total_pnl = sum(t.get("pnl", 0) for t in trades)
    total_fees = sum(t.get("fee", 0) for t in trades)
    gross_profit = sum(t.get("pnl", 0) for t in wins) if wins else 0
    gross_loss = abs(sum(t.get("pnl", 0) for t in losses)) if losses else 0

    win_rate = len(wins) / len(trades) if trades else 0
    avg_win = gross_profit / len(wins) if wins else 0
    avg_loss = gross_loss / len(losses) if losses else 0
    profit_factor = gross_profit / gross_loss if gross_loss > 0 else float("inf")

    # Net expectancy (report recommendation #1)
    expectancy = (win_rate * avg_win) - ((1 - win_rate) * avg_loss) if trades else 0

    # Fee burden (report finding: 32% of loss is fees)
    fee_burden_pct = abs(total_fees / total_pnl * 100) if total_pnl != 0 else 0

    # Tier distribution
    tier_stats = {}
    for t in trades:
        tier = t.get("tier", t.get("grade", "?"))
        if tier not in tier_stats:
            tier_stats[tier] = {"count": 0, "pnl": 0, "fees": 0}
        tier_stats[tier]["count"] += 1
        tier_stats[tier]["pnl"] += t.get("pnl", 0)
        tier_stats[tier]["fees"] += t.get("fee", 0)

    # Timeframe distribution
    tf_stats = {}
    for t in trades:
        tf = str(t.get("timeframe", "?"))
        if tf not in tf_stats:
            tf_stats[tf] = {"count": 0, "pnl": 0, "wins": 0}
        tf_stats[tf]["count"] += 1
        tf_stats[tf]["pnl"] += t.get("pnl", 0)
        if t.get("pnl", 0) > 0:
            tf_stats[tf]["wins"] += 1

    # Realized RR tracking
    rr_values = [t.get("realized_rr", 0) for t in trades if t.get("realized_rr")]
    avg_rr = sum(rr_values) / len(rr_values) if rr_values else 0

    return {
        "count": len(trades),
        "wins": len(wins),
        "losses": len(losses),
        "win_rate": win_rate,
        "total_pnl": total_pnl,
        "total_fees": total_fees,
        "gross_profit": gross_profit,
        "gross_loss": gross_loss,
        "avg_win": avg_win,
        "avg_loss": avg_loss,
        "profit_factor": profit_factor,
        "expectancy": expectancy,
        "fee_burden_pct": fee_burden_pct,
        "avg_realized_rr": avg_rr,
        "tier_stats": tier_stats,
        "tf_stats": tf_stats,
    }


def analyze_24h_trades(trades: list) -> list:
    """Filter trades from last 24 hours."""
    now = datetime.now(timezone.utc)
    cutoff = now - timedelta(hours=24)
    recent = []
    for t in trades:
        ts_str = t.get("close_time", t.get("timestamp", ""))
        if not ts_str:
            continue
        try:
            if isinstance(ts_str, (int, float)):
                ts = datetime.fromtimestamp(ts_str, tz=timezone.utc)
            else:
                ts_str_clean = ts_str.replace("Z", "+00:00")
                ts = datetime.fromisoformat(ts_str_clean)
                if ts.tzinfo is None:
                    ts = ts.replace(tzinfo=timezone.utc)
            if ts >= cutoff:
                recent.append(t)
        except (ValueError, TypeError):
            continue
    return recent


def get_readiness_info() -> dict:
    """Get AUTO-LIVE pipeline status."""
    return api_get("/api/readiness")


def build_report() -> str:
    """Build the daily quality report text."""
    now_kst = datetime.now(KST)
    header = f"<b>Daily Trade Quality Report</b>\n{now_kst.strftime('%Y-%m-%d %H:%M KST')}\n"

    # --- Get data ---
    stats = api_get("/api/stats")
    shadow_stats = api_get("/api/shadow/stats")
    readiness = get_readiness_info()
    scores_data = api_get("/api/symbols/scores")

    # Shadow state for trade details
    shadow_state = load_state_file("shadow_state.json")
    all_trades = shadow_state.get("trades", [])
    recent_trades = analyze_24h_trades(all_trades)

    # --- All-time analysis ---
    all_analysis = analyze_trades(all_trades)
    recent_analysis = analyze_trades(recent_trades)

    lines = [header, ""]

    # === SECTION 1: Overall Status ===
    lines.append("<b>[Overall Status]</b>")
    if stats:
        lines.append(f"  Total Trades: {stats.get('total_closes', all_analysis['count'])}")
        lines.append(f"  Win Rate: {all_analysis['win_rate']:.1%}")
        lines.append(f"  Total PnL: ${all_analysis['total_pnl']:.2f}")
        lines.append(f"  Total Fees: ${all_analysis['total_fees']:.2f}")
        if all_analysis['total_pnl'] != 0:
            lines.append(f"  Fee Burden: {all_analysis['fee_burden_pct']:.1f}% of PnL")
        lines.append(f"  Profit Factor: {all_analysis['profit_factor']:.2f}")
        lines.append(f"  Expectancy: ${all_analysis['expectancy']:.4f}")
    lines.append("")

    # === SECTION 2: 24h Performance ===
    lines.append("<b>[24h Performance]</b>")
    if recent_analysis["count"] > 0:
        r = recent_analysis
        lines.append(f"  Trades: {r['count']} (W:{r['wins']} / L:{r['losses']})")
        lines.append(f"  Win Rate: {r['win_rate']:.1%}")
        lines.append(f"  PnL: ${r['total_pnl']:.2f}")
        lines.append(f"  Fees: ${r['total_fees']:.2f}")
        lines.append(f"  Avg Win: ${r['avg_win']:.4f} / Avg Loss: ${r['avg_loss']:.4f}")
        if r['avg_loss'] > 0:
            rr = r['avg_win'] / r['avg_loss']
            lines.append(f"  Win/Loss Ratio: {rr:.2f}")
    else:
        lines.append("  No trades in last 24h")
    lines.append("")

    # === SECTION 3: Tier Distribution (KEY - report says D-tier = main loss source) ===
    lines.append("<b>[Tier Performance]</b>")
    tier_order = ["S", "A", "B", "C", "D", "X", "F", "?"]
    for tier in tier_order:
        if tier in all_analysis.get("tier_stats", {}):
            ts = all_analysis["tier_stats"][tier]
            lines.append(f"  {tier}: {ts['count']} trades, PnL ${ts['pnl']:.2f}, Fees ${ts['fees']:.2f}")
    lines.append("")

    # === SECTION 4: Timeframe Performance ===
    lines.append("<b>[Timeframe Performance]</b>")
    for tf in sorted(all_analysis.get("tf_stats", {}).keys(), key=lambda x: int(x) if x.isdigit() else 999):
        ts = all_analysis["tf_stats"][tf]
        wr = ts["wins"] / ts["count"] if ts["count"] > 0 else 0
        lines.append(f"  {tf}m: {ts['count']} trades, WR {wr:.0%}, PnL ${ts['pnl']:.2f}")
    lines.append("")

    # === SECTION 5: Cost Analysis (report key finding: 32% fee burden) ===
    lines.append("<b>[Cost Analysis]</b>")
    if all_analysis["count"] > 0:
        avg_fee_per_trade = all_analysis["total_fees"] / all_analysis["count"]
        lines.append(f"  Avg Fee/Trade: ${avg_fee_per_trade:.4f}")
        if all_analysis["gross_loss"] > 0:
            fee_vs_loss = abs(all_analysis["total_fees"]) / all_analysis["gross_loss"] * 100
            lines.append(f"  Fees vs Gross Loss: {fee_vs_loss:.1f}%")
        # Net expectancy with/without fees
        net_exp = all_analysis["expectancy"]
        gross_exp = net_exp + avg_fee_per_trade
        lines.append(f"  Gross Expectancy: ${gross_exp:.4f}")
        lines.append(f"  Net Expectancy: ${net_exp:.4f}")
        if gross_exp > 0 and net_exp < 0:
            lines.append("  ⚠ Fees are erasing positive edge!")
    lines.append("")

    # === SECTION 6: AUTO-LIVE Pipeline ===
    lines.append("<b>[AUTO-LIVE Pipeline]</b>")
    if readiness:
        pipeline = readiness.get("pipeline", {})
        eligible = readiness.get("eligible_count", 0)
        live = readiness.get("live_mode", False)
        lines.append(f"  B={pipeline.get('blocked', 0)} L={pipeline.get('learning', 0)} "
                     f"P={pipeline.get('promising', 0)} R={pipeline.get('ready', 0)} "
                     f"V={pipeline.get('proven', 0)}")
        lines.append(f"  Eligible: {eligible} | Live: {'YES' if live else 'NO'}")
    lines.append("")

    # === SECTION 7: Improvement Recommendations ===
    lines.append("<b>[Auto Recommendations]</b>")
    recs = []
    if all_analysis["fee_burden_pct"] > 25:
        recs.append(f"- Fee burden {all_analysis['fee_burden_pct']:.0f}% > 25% target. Consider maker orders.")
    if all_analysis["expectancy"] < 0:
        recs.append(f"- Negative expectancy ${all_analysis['expectancy']:.4f}. Review signal quality.")
    if all_analysis["profit_factor"] < 1.2:
        recs.append(f"- Profit factor {all_analysis['profit_factor']:.2f} < 1.2 target.")
    # D-tier check
    d_stats = all_analysis.get("tier_stats", {}).get("D", {})
    if d_stats and d_stats.get("pnl", 0) < -10:
        recs.append(f"- D-tier loss ${d_stats['pnl']:.2f}: primary loss driver.")
    if not recs:
        recs.append("- All metrics within targets.")
    lines.extend(recs)

    return "\n".join(lines)


def save_history(analysis: dict):
    """Save daily metrics for trend tracking."""
    history = []
    if REPORT_HISTORY.exists():
        try:
            with open(REPORT_HISTORY, "r") as f:
                history = json.load(f)
        except Exception:
            history = []

    entry = {
        "date": datetime.now(KST).strftime("%Y-%m-%d"),
        "count": analysis.get("count", 0),
        "win_rate": round(analysis.get("win_rate", 0), 4),
        "total_pnl": round(analysis.get("total_pnl", 0), 4),
        "total_fees": round(analysis.get("total_fees", 0), 4),
        "expectancy": round(analysis.get("expectancy", 0), 6),
        "profit_factor": round(analysis.get("profit_factor", 0), 4),
        "fee_burden_pct": round(analysis.get("fee_burden_pct", 0), 2),
    }
    history.append(entry)

    # Keep last 90 days
    history = history[-90:]

    try:
        with open(REPORT_HISTORY, "w") as f:
            json.dump(history, f, indent=2)
    except Exception as e:
        log.error(f"Failed to save history: {e}")


def main():
    log.info("Generating daily trade quality report...")

    # Build and send report
    report = build_report()
    send_telegram(report)

    # Save metrics history for trend tracking
    shadow_state = load_state_file("shadow_state.json")
    all_trades = shadow_state.get("trades", [])
    analysis = analyze_trades(all_trades)
    save_history(analysis)

    log.info("Report complete.")


if __name__ == "__main__":
    main()

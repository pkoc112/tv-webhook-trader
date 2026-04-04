#!/bin/bash
# ============================================================================
# watchdog.sh — 시스템 감시자
# 5분마다 cron으로 실행. 서비스 상태 확인 + 자동 복구 + 텔레그램 알림
# ============================================================================

set -euo pipefail

# ── 설정 ──
SERVICE="tv-webhook-trader"
TELEGRAM_SERVICE="telegram-alert"
HEALTH_URL="http://localhost:5000/health"
API_URL="http://localhost:5000/api"
AUTH_HEADER="Authorization: Basic $(echo -n 'admin:changeme' | base64)"
LOGFILE="/home/ubuntu/tv-webhook-trader/data/watchdog.log"
DAILY_REPORT_FLAG="/tmp/watchdog_daily_sent"

# 텔레그램 설정 (.env에서 로드)
source /home/ubuntu/tv-webhook-trader/.env 2>/dev/null || true

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $1" >> "$LOGFILE"
}

send_telegram() {
    local msg="$1"
    if [ -n "$TELEGRAM_BOT_TOKEN" ] && [ -n "$TELEGRAM_CHAT_ID" ]; then
        curl -s --max-time 10 \
            -X POST "https://api.telegram.org/bot${TELEGRAM_BOT_TOKEN}/sendMessage" \
            -d "chat_id=${TELEGRAM_CHAT_ID}" \
            -d "text=${msg}" \
            -d "parse_mode=HTML" > /dev/null 2>&1 || true
    fi
}

# ── 1. 트레이더 서비스 체크 ──
check_trader() {
    if ! systemctl is-active --quiet "$SERVICE"; then
        log "ALERT: $SERVICE is DOWN. Restarting..."
        send_telegram "🔴 <b>서비스 다운!</b>
tv-webhook-trader가 중지됨.
자동 재시작 시도 중..."

        sudo systemctl start "$SERVICE"
        sleep 5

        if systemctl is-active --quiet "$SERVICE"; then
            log "OK: $SERVICE restarted successfully"
            send_telegram "🟢 <b>서비스 복구!</b>
tv-webhook-trader 자동 재시작 성공."
        else
            log "CRITICAL: $SERVICE restart FAILED"
            send_telegram "🔴🔴 <b>재시작 실패!</b>
tv-webhook-trader를 시작할 수 없습니다.
수동 확인 필요!"
        fi
        return
    fi

    # 서비스는 active인데 대시보드가 응답하는지 확인
    local health
    health=$(curl -s --max-time 5 "$HEALTH_URL" 2>/dev/null || echo "TIMEOUT")

    if [ "$health" != '{"status":"ok"}' ]; then
        log "ALERT: Dashboard not responding ($health). Force restarting..."
        send_telegram "⚠️ <b>대시보드 응답 없음!</b>
서비스는 active이나 HTTP 응답 실패.
강제 재시작 시도 중..."

        sudo systemctl kill -s SIGKILL "$SERVICE" 2>/dev/null || true
        sleep 2
        sudo systemctl start "$SERVICE"
        sleep 5

        health=$(curl -s --max-time 5 "$HEALTH_URL" 2>/dev/null || echo "TIMEOUT")
        if [ "$health" = '{"status":"ok"}' ]; then
            log "OK: Dashboard recovered after force restart"
            send_telegram "🟢 <b>대시보드 복구!</b>
강제 재시작 후 정상 응답."
        else
            log "CRITICAL: Dashboard still not responding after restart"
            send_telegram "🔴 <b>대시보드 복구 실패!</b>
수동 확인 필요!"
        fi
    else
        log "OK: Trader service healthy"
    fi
}

# ── 2. 텔레그램 서비스 체크 ──
check_telegram() {
    if ! systemctl is-active --quiet "$TELEGRAM_SERVICE"; then
        log "ALERT: $TELEGRAM_SERVICE is DOWN. Starting..."
        sudo systemctl start "$TELEGRAM_SERVICE"
        sleep 3
        if systemctl is-active --quiet "$TELEGRAM_SERVICE"; then
            log "OK: $TELEGRAM_SERVICE started"
        else
            log "WARN: $TELEGRAM_SERVICE failed to start"
        fi
    fi
}

# ── 3. 일일 요약 리포트 (매일 09:00 KST = 00:00 UTC) ──
daily_report() {
    local hour=$(date -u '+%H')
    local minute=$(date -u '+%M')

    # UTC 00:00~00:09 사이에 한 번만 발송
    if [ "$hour" = "00" ] && [ "$minute" -lt "10" ]; then
        if [ -f "$DAILY_REPORT_FLAG" ]; then
            local flag_date=$(cat "$DAILY_REPORT_FLAG")
            local today=$(date '+%Y-%m-%d')
            if [ "$flag_date" = "$today" ]; then
                return  # 이미 오늘 발송함
            fi
        fi

        # API에서 데이터 수집
        local stats
        stats=$(curl -s --max-time 10 -H "$AUTH_HEADER" "$API_URL/stats" 2>/dev/null || echo "{}")

        local live_equiv
        live_equiv=$(curl -s --max-time 10 -H "$AUTH_HEADER" "$API_URL/shadow/live-equiv-stats" 2>/dev/null || echo "{}")

        local balance=$(echo "$stats" | python3 -c "import json,sys;d=json.load(sys.stdin);print(d.get('balance',0))" 2>/dev/null || echo "?")
        local trades=$(echo "$stats" | python3 -c "import json,sys;d=json.load(sys.stdin);print(d.get('total_trades',0))" 2>/dev/null || echo "?")
        local wr=$(echo "$stats" | python3 -c "import json,sys;d=json.load(sys.stdin);print(d.get('win_rate',0))" 2>/dev/null || echo "?")
        local positions=$(echo "$stats" | python3 -c "import json,sys;d=json.load(sys.stdin);print(d.get('open_positions',0))" 2>/dev/null || echo "?")
        local skips=$(echo "$stats" | python3 -c "import json,sys;d=json.load(sys.stdin);print(d.get('risk_skips',0))" 2>/dev/null || echo "?")

        local le_closes=$(echo "$live_equiv" | python3 -c "import json,sys;d=json.load(sys.stdin);print(d.get('total_closes',0))" 2>/dev/null || echo "?")
        local le_pnl=$(echo "$live_equiv" | python3 -c "import json,sys;d=json.load(sys.stdin);print(f\"{d.get('total_pnl',0):+.4f}\")" 2>/dev/null || echo "?")
        local le_wr=$(echo "$live_equiv" | python3 -c "import json,sys;d=json.load(sys.stdin);print(d.get('win_rate',0))" 2>/dev/null || echo "?")

        local uptime=$(systemctl show "$SERVICE" --property=ActiveEnterTimestamp --value 2>/dev/null || echo "?")

        send_telegram "📊 <b>일일 리포트</b> ($(date '+%Y-%m-%d'))

💰 잔고: \$${balance}
📈 거래: ${trades}건 | 승률: ${wr}%
📂 포지션: ${positions}개 | 스킵: ${skips}건

🎯 <b>라이브 등가</b>
청산: ${le_closes}건 | PnL: ${le_pnl}
승률: ${le_wr}%

⏱ 서비스 시작: ${uptime}
🟢 시스템 정상 작동 중

<i>규칙 동결 중 — 100건 live-equiv까지 대기</i>"

        date '+%Y-%m-%d' > "$DAILY_REPORT_FLAG"
        log "OK: Daily report sent"
    fi
}

# ── 4. 100건 자동 판정 + auto-live 전환 ──
MILESTONE_FLAG="/tmp/watchdog_100_milestone"
LIVE_TRANSITION_FLAG="/home/ubuntu/tv-webhook-trader/data/auto_live_approved"
check_milestone() {
    # 이미 판정 완료했으면 스킵
    if [ -f "$MILESTONE_FLAG" ]; then
        return
    fi

    local le
    le=$(curl -s --max-time 10 -H "$AUTH_HEADER" "$API_URL/shadow/live-equiv-stats" 2>/dev/null || echo "{}")
    local closes=$(echo "$le" | python3 -c "import json,sys;d=json.load(sys.stdin);print(d.get('total_closes',0))" 2>/dev/null || echo "0")

    if [ "$closes" -lt 100 ]; then
        # 25건, 50건, 75건 마일스톤 알림
        for milestone in 25 50 75; do
            local ms_flag="/tmp/watchdog_ms_${milestone}"
            if [ "$closes" -ge "$milestone" ] && [ ! -f "$ms_flag" ]; then
                local le_pnl=$(echo "$le" | python3 -c "import json,sys;d=json.load(sys.stdin);print(f\"{d.get('total_pnl',0):+.4f}\")" 2>/dev/null || echo "?")
                local le_wr=$(echo "$le" | python3 -c "import json,sys;d=json.load(sys.stdin);print(d.get('win_rate',0))" 2>/dev/null || echo "?")
                send_telegram "📍 <b>마일스톤: ${milestone}건 도달!</b>
PnL: ${le_pnl} | 승률: ${le_wr}%
진행: ${closes}/100건 (${milestone}%)"
                touch "$ms_flag"
                log "Milestone ${milestone} reached: closes=${closes}"
            fi
        done
        return
    fi

    # ═══ 100건 도달! 자동 판정 ═══
    log "MILESTONE: 100 live-equiv closed trades reached!"

    # 판정 데이터 수집
    local le_trades
    le_trades=$(curl -s --max-time 10 -H "$AUTH_HEADER" "$API_URL/shadow/live-equiv-trades" 2>/dev/null || echo "[]")

    local judgment
    judgment=$(python3 -c "
import json, sys

le = json.loads('${le//\'/\\\'}')
trades = json.loads(sys.stdin.read())

closes = le.get('total_closes', 0)
wins = le.get('wins', 0)
losses = le.get('losses', 0)
wr = le.get('win_rate', 0)
total_pnl = le.get('total_pnl', 0)

# Gross/Net 분리
gross_sum = sum(t.get('gross_pnl', 0) for t in trades)
fee_sum = sum(t.get('fee_cost', 0) for t in trades)
net_sum = sum(t.get('net_pnl', 0) for t in trades)

# 예상 실거래 수수료 (taker 0.12% 왕복, avg notional $5.50)
est_fee_per_trade = 5.50 * 0.0012
est_total_fee = est_fee_per_trade * closes
est_actual_net = gross_sum - est_total_fee

avg_gross = gross_sum / closes if closes > 0 else 0
avg_net = net_sum / closes if closes > 0 else 0

# 판정 로직
verdict = 'UNKNOWN'
detail = ''
auto_live = False

if gross_sum > 0 and est_actual_net > 0:
    verdict = 'PASS'
    detail = 'gross>0 AND estimated_net>0'
elif gross_sum > 0 and est_actual_net <= 0:
    verdict = 'REVIEW'
    detail = 'gross>0 BUT net<0 -> execution/cost fix needed'
elif gross_sum <= 0:
    verdict = 'FAIL'
    detail = 'gross<=0 -> signal family review needed'

# Payoff ratio (GPT Gate 1)
wins_list = [t.get('net_pnl', 0) for t in trades if t.get('net_pnl', 0) > 0]
losses_list = [t.get('net_pnl', 0) for t in trades if t.get('net_pnl', 0) <= 0]
avg_win = sum(wins_list) / len(wins_list) if wins_list else 0
avg_loss = sum(losses_list) / len(losses_list) if losses_list else 0
payoff = abs(avg_win / avg_loss) if avg_loss != 0 else 0
payoff_check = 'PASS' if payoff >= 1.0 else 'FAIL'

# 50-trade block consistency
block1_pnl = sum(t.get('net_pnl', 0) for t in trades[:50])
block2_pnl = sum(t.get('net_pnl', 0) for t in trades[50:100])
consistency = 'PASS' if block1_pnl > 0 and block2_pnl > 0 else 'FAIL'

# Symbol concentration (GPT: 25% threshold)
from collections import Counter
symbols = Counter(t.get('symbol', '?') for t in trades)
top_symbol, top_count = symbols.most_common(1)[0] if symbols else ('?', 0)
concentration = top_count / closes * 100 if closes > 0 else 0
conc_check = 'PASS' if concentration < 25 else 'WARN'

# Cost drag ratio (GPT: <35%)
cost_drag = est_total_fee / gross_sum * 100 if gross_sum > 0 else 100
cost_check = 'PASS' if cost_drag < 35 else 'WARN'

# Final verdict with all gates
all_pass = (verdict == 'PASS' and consistency == 'PASS'
            and payoff_check == 'PASS' and conc_check == 'PASS')

print(f'verdict={verdict}')
print(f'all_pass={all_pass}')
print(f'closes={closes}')
print(f'wr={wr}')
print(f'gross={gross_sum:+.4f}')
print(f'est_fee={est_total_fee:.4f}')
print(f'est_net={est_actual_net:+.4f}')
print(f'avg_gross={avg_gross:+.4f}')
print(f'avg_win={avg_win:+.4f}')
print(f'avg_loss={avg_loss:+.4f}')
print(f'payoff={payoff:.2f}')
print(f'payoff_check={payoff_check}')
print(f'block1={block1_pnl:+.4f}')
print(f'block2={block2_pnl:+.4f}')
print(f'consistency={consistency}')
print(f'top_symbol={top_symbol}({concentration:.0f}%)')
print(f'conc_check={conc_check}')
print(f'cost_drag={cost_drag:.1f}%')
print(f'cost_check={cost_check}')
print(f'detail={detail}')
" <<< "$le_trades" 2>/dev/null || echo "verdict=ERROR")

    # 판정 결과 파싱
    local verdict=$(echo "$judgment" | grep "^verdict=" | cut -d= -f2)
    local auto_live=$(echo "$judgment" | grep "^auto_live=" | cut -d= -f2)
    local wr=$(echo "$judgment" | grep "^wr=" | cut -d= -f2)
    local gross=$(echo "$judgment" | grep "^gross=" | cut -d= -f2)
    local est_net=$(echo "$judgment" | grep "^est_net=" | cut -d= -f2)
    local est_fee=$(echo "$judgment" | grep "^est_fee=" | cut -d= -f2)
    local block1=$(echo "$judgment" | grep "^block1=" | cut -d= -f2)
    local block2=$(echo "$judgment" | grep "^block2=" | cut -d= -f2)
    local consistency=$(echo "$judgment" | grep "^consistency=" | cut -d= -f2)
    local top_sym=$(echo "$judgment" | grep "^top_symbol=" | cut -d= -f2)
    local conc_check=$(echo "$judgment" | grep "^conc_check=" | cut -d= -f2)
    local detail=$(echo "$judgment" | grep "^detail=" | cut -d= -f2-)

    # 추가 판정 값 파싱
    local all_pass=$(echo "$judgment" | grep "^all_pass=" | cut -d= -f2)
    local payoff=$(echo "$judgment" | grep "^payoff=" | cut -d= -f2)
    local payoff_check=$(echo "$judgment" | grep "^payoff_check=" | cut -d= -f2)
    local avg_win=$(echo "$judgment" | grep "^avg_win=" | cut -d= -f2)
    local avg_loss=$(echo "$judgment" | grep "^avg_loss=" | cut -d= -f2)
    local cost_drag=$(echo "$judgment" | grep "^cost_drag=" | cut -d= -f2)
    local cost_check=$(echo "$judgment" | grep "^cost_check=" | cut -d= -f2)

    local emoji="❓"
    if [ "$all_pass" = "True" ]; then emoji="🟢"; fi
    if [ "$verdict" = "REVIEW" ]; then emoji="🟡"; fi
    if [ "$verdict" = "FAIL" ]; then emoji="🔴"; fi
    if [ "$all_pass" = "False" ] && [ "$verdict" = "PASS" ]; then emoji="🟡"; fi

    send_telegram "${emoji} <b>100건 판정 완료!</b>

<b>최종: $([ "$all_pass" = "True" ] && echo "ALL PASS" || echo "REVIEW NEEDED")</b>
${detail}

📊 <b>성과</b>
승률: ${wr}%
Gross: ${gross} | Net: ${est_net}
수수료: -${est_fee}
Avg Win: ${avg_win} | Avg Loss: ${avg_loss}
Payoff: ${payoff} (${payoff_check})

📈 <b>검증</b>
Block 1 (1-50): ${block1}
Block 2 (51-100): ${block2}
일관성: ${consistency}
심볼 집중: ${top_sym} (${conc_check})
비용 드래그: ${cost_drag} (${cost_check})

$([ "$all_pass" = "True" ] && echo "✅ <b>LIVE 전환 승인 대기</b>
성훈님이 직접 확인 후 수동 전환해주세요.
Gate 1~4 체크 후 승인하세요." || echo "⏸ <b>추가 검토 필요</b>
일부 조건 미달. 수동 확인 후 판단하세요.")

<i>자동 전환 없음 - 수동 승인 필요</i>"

    log "100-TRADE JUDGMENT: verdict=${verdict} all_pass=${all_pass} gross=${gross} est_net=${est_net} payoff=${payoff} consistency=${consistency} conc=${top_sym} cost_drag=${cost_drag}"

    touch "$MILESTONE_FLAG"
}

# ── 5. 킬 스위치 (긴급 거래 중단) ──
# 일일 손실 한도 초과, 비정상 드로다운, 급격한 잔고 감소 감지
KILL_SWITCH_FILE="/home/ubuntu/tv-webhook-trader/data/kill_switch"
check_kill_switch() {
    # 킬스위치 파일이 있으면 서비스 중지 유지
    if [ -f "$KILL_SWITCH_FILE" ]; then
        if systemctl is-active --quiet "$SERVICE"; then
            log "KILL SWITCH ACTIVE: Stopping service"
            sudo systemctl stop "$SERVICE"
            send_telegram "🛑 <b>킬 스위치 활성!</b>
kill_switch 파일 감지. 서비스 중지됨.
수동 해제: rm $KILL_SWITCH_FILE"
        fi
        return
    fi

    # API에서 리스크 상태 확인
    local risk
    risk=$(curl -s --max-time 5 -H "$AUTH_HEADER" "$API_URL/risk/status" 2>/dev/null || echo "{}")

    if [ "$risk" = "{}" ]; then
        return  # API 응답 없으면 스킵
    fi

    local dd=$(echo "$risk" | python3 -c "import json,sys;d=json.load(sys.stdin);print(d.get('portfolio',{}).get('current_drawdown_pct',0))" 2>/dev/null || echo "0")
    local cb=$(echo "$risk" | python3 -c "import json,sys;d=json.load(sys.stdin);print(d.get('portfolio',{}).get('circuit_breaker_active',False))" 2>/dev/null || echo "False")

    # Validation DD 10% → shadow 복귀 경고 (GPT 권고: 검증 단계에서 8~10%)
    if [ "$(echo "$dd > 10" | bc -l 2>/dev/null || echo 0)" = "1" ]; then
        local val_flag="/tmp/watchdog_validation_dd_warn"
        if [ ! -f "$val_flag" ]; then
            log "WARN: Validation DD ${dd}% > 10%"
            send_telegram "⚠️ <b>Validation DD 경고!</b>
드로다운 ${dd}% (검증 한도 10% 초과)
검증 단계에서는 8~10%가 shadow 복귀 기준입니다.

Live 전환 후라면 shadow 복귀를 검토하세요.
Shadow 모드에서는 참고 알림입니다."
            touch "$val_flag"
        fi
    fi

    # 드로다운 30% 초과 → 긴급 정지
    if [ "$(echo "$dd > 30" | bc -l 2>/dev/null || echo 0)" = "1" ]; then
        log "KILL SWITCH TRIGGERED: Drawdown ${dd}% > 30%"
        echo "DD_${dd}_$(date '+%Y%m%d_%H%M%S')" > "$KILL_SWITCH_FILE"
        sudo systemctl stop "$SERVICE"
        send_telegram "🛑🛑 <b>긴급 정지!</b>
드로다운 ${dd}% (한도 30% 초과)
서비스가 자동 중지되었습니다.
재개: rm $KILL_SWITCH_FILE 후 systemctl start"
        return
    fi

    # 서킷브레이커 발동 알림
    if [ "$cb" = "True" ]; then
        log "WARN: Circuit breaker active"
        send_telegram "⚠️ <b>서킷브레이커 발동!</b>
일일/주간 손실 한도에 도달.
자동 쿨다운 중 (60분)."
    fi
}

# ── 5. 로그 로테이션 (1MB 초과 시) ──
rotate_log() {
    if [ -f "$LOGFILE" ]; then
        local size=$(stat -f%z "$LOGFILE" 2>/dev/null || stat -c%s "$LOGFILE" 2>/dev/null || echo 0)
        if [ "$size" -gt 1048576 ]; then
            mv "$LOGFILE" "${LOGFILE}.old"
            log "Log rotated"
        fi
    fi
}

# ── 메인 ──
mkdir -p "$(dirname "$LOGFILE")"
rotate_log
check_kill_switch
check_trader
check_telegram
check_milestone
daily_report

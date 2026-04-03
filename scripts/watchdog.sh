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

# ── 4. 킬 스위치 (긴급 거래 중단) ──
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
daily_report

#!/bin/bash
# ============================================================================
# preflight_check.sh -- 실거래 전 사전 점검 스크립트
# ============================================================================
set -e

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
CONFIG="$PROJECT_DIR/config/config.json"
API_KEYS="$PROJECT_DIR/config/api_keys.json"
STATE="$PROJECT_DIR/../sfx-trader/data/state.json"

PASS=0
FAIL=0
WARN=0

check_pass() { echo "  ✅ $1"; ((PASS++)); }
check_fail() { echo "  ❌ $1"; ((FAIL++)); }
check_warn() { echo "  ⚠️  $1"; ((WARN++)); }

echo "=========================================="
echo "  실거래 사전 점검 (Preflight Check)"
echo "=========================================="
echo ""

# -- 1. 바이너리 존재 --
echo "[1] 빌드 확인"
if [ -f "$PROJECT_DIR/build/tv_webhook_trader" ]; then
    check_pass "메인 서버 바이너리 존재"
else
    check_fail "메인 서버 바이너리 없음 (build 필요)"
fi
if [ -f "$PROJECT_DIR/build/backtest" ]; then
    check_pass "백테스터 바이너리 존재"
else
    check_warn "백테스터 바이너리 없음"
fi

# -- 2. 설정 파일 --
echo ""
echo "[2] 설정 파일"
if [ -f "$CONFIG" ]; then
    check_pass "config.json 존재"

    # auth_token 체크
    TOKEN=$(python3 -c "import json; print(json.load(open('$CONFIG'))['webhook']['auth_token'])" 2>/dev/null)
    if [ "$TOKEN" = "YOUR_SECRET_TOKEN_HERE" ] || [ "$TOKEN" = "CHANGE_ME" ] || [ -z "$TOKEN" ]; then
        check_fail "auth_token 미설정 (기본값 사용 중)"
    else
        check_pass "auth_token 설정됨 (${#TOKEN}자)"
    fi

    # 심볼 수
    SYM_COUNT=$(python3 -c "import json; print(len(json.load(open('$CONFIG'))['trading']['symbols']))" 2>/dev/null || echo "0")
    check_pass "심볼 수: $SYM_COUNT"

    # trade_amount_usdt
    AMOUNT=$(python3 -c "import json; print(json.load(open('$CONFIG'))['trading']['trade_amount_usdt'])" 2>/dev/null)
    check_pass "건당 투입: $AMOUNT USDT"
else
    check_fail "config.json 없음"
fi

# -- 3. API 키 --
echo ""
echo "[3] API 키"
if [ -f "$API_KEYS" ]; then
    check_pass "api_keys.json 존재"
    KEY=$(python3 -c "import json; k=json.load(open('$API_KEYS'))['api_key']; print(k[:10]+'...' if len(k)>10 else k)" 2>/dev/null)
    check_pass "API Key: $KEY"
else
    check_fail "api_keys.json 없음"
fi

# -- 4. SSL 인증서 --
echo ""
echo "[4] SSL 인증서"
SSL_CERT=$(python3 -c "import json; print(json.load(open('$CONFIG'))['webhook']['ssl_cert'])" 2>/dev/null)
if [ -f "$SSL_CERT" ]; then
    EXPIRY=$(openssl x509 -enddate -noout -in "$SSL_CERT" 2>/dev/null | cut -d= -f2)
    check_pass "SSL 인증서 존재 (만료: $EXPIRY)"
else
    check_warn "SSL 인증서 없음: $SSL_CERT (배포 서버에서 확인)"
fi

# -- 5. Paper Trading 데이터 --
echo ""
echo "[5] Paper Trading 데이터"
if [ -f "$STATE" ]; then
    SIG=$(python3 -c "import json; print(len(json.load(open('$STATE')).get('signals',[])))" 2>/dev/null || echo "0")
    TRADES=$(python3 -c "import json; print(json.load(open('$STATE')).get('total_trades',0))" 2>/dev/null || echo "0")
    PNL=$(python3 -c "import json; print(f\"{json.load(open('$STATE')).get('total_pnl',0):.2f}\")" 2>/dev/null || echo "0")

    check_pass "시그널: $SIG개 | 거래: ${TRADES}건 | 수익: ${PNL} USDT"

    if [ "$SIG" -lt 100 ]; then
        check_warn "시그널 100개 미만 - WF 최적화 신뢰도 낮음"
    else
        check_pass "WF 최적화 가능 ($SIG >= 100)"
    fi
else
    check_warn "state.json 없음 (Paper Trading 미실행)"
fi

# -- 6. 네트워크 --
echo ""
echo "[6] 네트워크"
if curl -s --max-time 5 "https://api.bitget.com/api/v2/public/time" > /dev/null 2>&1; then
    check_pass "Bitget API 접속 가능"
else
    check_warn "Bitget API 접속 불가 (방화벽 또는 네트워크 확인)"
fi

# -- 결과 요약 --
echo ""
echo "=========================================="
echo "  결과: ✅ $PASS 통과 | ❌ $FAIL 실패 | ⚠️  $WARN 경고"
echo "=========================================="

if [ "$FAIL" -gt 0 ]; then
    echo ""
    echo "  ❌ 실패 항목을 해결한 후 실거래를 시작하세요."
    exit 1
else
    echo ""
    echo "  실거래 준비 완료! 🚀"
    echo ""
    echo "  실행 순서:"
    echo "    1. WF 최적화: ./scripts/run_wf.sh --apply"
    echo "    2. 서버 시작: ./build/tv_webhook_trader config/config.json"
    echo "    3. TradingView에서 웹훅 알림 활성화"
fi

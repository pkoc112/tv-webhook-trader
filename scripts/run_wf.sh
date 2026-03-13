#!/bin/bash
# ============================================================================
# run_wf.sh -- Walk-Forward 최적화 실행 & config 자동 반영
#
# 사용법:
#   ./scripts/run_wf.sh                    # 기본 (WF 최적화만)
#   ./scripts/run_wf.sh --apply            # WF 후 config.json에 자동 반영
#   ./scripts/run_wf.sh --symbol BTCUSDT   # 특정 심볼만
# ============================================================================
set -e

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BACKTEST="$PROJECT_DIR/build/backtest"
STATE_JSON="$PROJECT_DIR/../sfx-trader/data/state.json"
CONFIG_JSON="$PROJECT_DIR/config/config.json"

# 백테스터 존재 확인
if [ ! -f "$BACKTEST" ]; then
    echo "ERROR: backtest binary not found. Build first:"
    echo "  cd $PROJECT_DIR/build && cmake .. && make"
    exit 1
fi

# state.json 존재 확인
if [ ! -f "$STATE_JSON" ]; then
    echo "ERROR: state.json not found at $STATE_JSON"
    echo "  sfx-trader에서 Paper Trading 데이터를 먼저 수집하세요."
    exit 1
fi

# 시그널 수 확인
SIGNAL_COUNT=$(python3 -c "import json; d=json.load(open('$STATE_JSON')); print(len(d.get('signals',[])))" 2>/dev/null || echo "0")
echo "현재 시그널 수: $SIGNAL_COUNT"

if [ "$SIGNAL_COUNT" -lt 50 ]; then
    echo "⚠️  시그널이 50개 미만입니다. WF 최적화에는 최소 70개 이상 권장."
    echo "  sfx-trader를 더 실행하여 데이터를 축적하세요."
fi

# 실행
APPLY_FLAG=""
EXTRA_ARGS=""

for arg in "$@"; do
    if [ "$arg" = "--apply" ]; then
        APPLY_FLAG="--apply $CONFIG_JSON"
    else
        EXTRA_ARGS="$EXTRA_ARGS $arg"
    fi
done

echo ""
echo "=========================================="
echo "  Walk-Forward 최적화 실행"
echo "  State: $STATE_JSON"
echo "  Signals: $SIGNAL_COUNT"
echo "=========================================="
echo ""

$BACKTEST "$STATE_JSON" --mode wf $APPLY_FLAG $EXTRA_ARGS

echo ""
echo "완료. config.json 적용 여부: ${APPLY_FLAG:-(미적용, --apply 옵션으로 적용)}"

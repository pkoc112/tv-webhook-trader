#!/bin/bash
# ============================================================================
# deploy.sh -- TV Webhook Trader 서버 배포 스크립트
# Ubuntu/Debian 서버 기준
# ============================================================================
set -e

echo "=========================================="
echo "  TV Webhook Trader - 서버 배포"
echo "=========================================="

# -- 1. 도메인 설정 (수정 필요) --
DOMAIN="your-domain.com"
EMAIL="your-email@gmail.com"
WEBHOOK_PORT=8443
AUTH_TOKEN="$(openssl rand -hex 32)"

echo "[1/6] 도메인: $DOMAIN"
echo "[1/6] 인증 토큰 생성: $AUTH_TOKEN"
echo ""
echo "⚠️  이 토큰을 TradingView 웹훅 URL에 사용합니다."
echo "⚠️  config.json과 TradingView Alert 둘 다에 동일하게 설정하세요."
echo ""

# -- 2. 시스템 의존성 설치 --
echo "[2/6] 시스템 패키지 설치..."
sudo apt update
sudo apt install -y build-essential cmake libboost-all-dev libssl-dev \
    libfmt-dev libspdlog-dev nlohmann-json3-dev certbot

# -- 3. SSL 인증서 발급 (Let's Encrypt) --
echo "[3/6] SSL 인증서 발급..."
sudo certbot certonly --standalone -d "$DOMAIN" -m "$EMAIL" --agree-tos -n

SSL_CERT="/etc/letsencrypt/live/$DOMAIN/fullchain.pem"
SSL_KEY="/etc/letsencrypt/live/$DOMAIN/privkey.pem"

echo "  인증서: $SSL_CERT"
echo "  키:     $SSL_KEY"

# -- 4. 프로젝트 빌드 --
echo "[4/6] 프로젝트 빌드..."
cd "$(dirname "$0")/.."
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

echo "  메인 서버:  build/tv_webhook_trader"
echo "  백테스터:   build/backtest"

# -- 5. config.json 자동 설정 --
echo "[5/6] config.json 업데이트..."
cd ..

# jq로 config 업데이트 (jq 없으면 수동)
if command -v jq &> /dev/null; then
    jq --arg cert "$SSL_CERT" --arg key "$SSL_KEY" \
       --arg token "$AUTH_TOKEN" --arg domain "$DOMAIN" \
       '.webhook.ssl_cert = $cert |
        .webhook.ssl_key = $key |
        .webhook.auth_token = $token' \
        config/config.json > config/config.json.tmp
    mv config/config.json.tmp config/config.json
    echo "  config.json 자동 업데이트 완료"
else
    echo "  ⚠️  jq 미설치. config/config.json을 수동으로 수정하세요:"
    echo "    webhook.ssl_cert: $SSL_CERT"
    echo "    webhook.ssl_key:  $SSL_KEY"
    echo "    webhook.auth_token: $AUTH_TOKEN"
fi

# -- 6. systemd 서비스 등록 --
echo "[6/6] systemd 서비스 등록..."
sudo tee /etc/systemd/system/tv-trader.service > /dev/null << UNIT
[Unit]
Description=TV Webhook Trader
After=network.target

[Service]
Type=simple
User=$USER
WorkingDirectory=$(pwd)
ExecStart=$(pwd)/build/tv_webhook_trader config/config.json
Restart=always
RestartSec=5
LimitNOFILE=65535

[Install]
WantedBy=multi-user.target
UNIT

sudo systemctl daemon-reload
sudo systemctl enable tv-trader
sudo systemctl start tv-trader

echo ""
echo "=========================================="
echo "  배포 완료!"
echo "=========================================="
echo ""
echo "  서비스 상태: sudo systemctl status tv-trader"
echo "  로그 보기:   sudo journalctl -u tv-trader -f"
echo "  서비스 재시작: sudo systemctl restart tv-trader"
echo ""
echo "  ★ TradingView 웹훅 URL:"
echo "    https://$DOMAIN:$WEBHOOK_PORT/webhook"
echo ""
echo "  ★ 웹훅 JSON Body:"
echo '    {'
echo '      "token": "'$AUTH_TOKEN'",'
echo '      "ticker": "{{ticker}}",'
echo '      "action": "{{strategy.order.action}}",'
echo '      "current_rating": {{plot("Current Rating")}},'
echo '      "signal_direction": "{{plot("Signal Direction")}}",'
echo '      "tp1": {{plot("TP1")}},'
echo '      "tp2": {{plot("TP2")}},'
echo '      "tp3": {{plot("TP3")}},'
echo '      "sl": {{plot("SL")}},'
echo '      "close": {{close}}'
echo '    }'
echo ""
echo "  ★ API 키 설정:"
echo "    config/api_keys.json 에 Bitget API 키를 입력하세요"
echo "=========================================="

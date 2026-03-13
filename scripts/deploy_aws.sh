#!/bin/bash
# ============================================================================
# deploy_aws.sh — AWS EC2 초기 배포 스크립트
# v1.0 | 2026-03-13
# 사용법: ssh로 EC2에 접속 후 실행
# ============================================================================
set -e

DOMAIN="${1:?Usage: $0 <your-domain.com>}"
EMAIL="${2:?Usage: $0 <domain> <email>}"

echo "=== 1. 시스템 업데이트 ==="
sudo apt-get update && sudo apt-get upgrade -y

echo "=== 2. 빌드 의존성 설치 ==="
sudo apt-get install -y build-essential cmake g++ \
    libboost-all-dev libssl-dev libfmt-dev libspdlog-dev \
    git certbot

echo "=== 3. Let's Encrypt SSL 인증서 발급 ==="
sudo certbot certonly --standalone -d "$DOMAIN" --email "$EMAIL" --agree-tos --non-interactive
echo "SSL cert: /etc/letsencrypt/live/${DOMAIN}/fullchain.pem"

echo "=== 4. 프로젝트 빌드 ==="
cd /opt/tv-webhook-trader
./scripts/build.sh Release

echo "=== 5. systemd 서비스 등록 ==="
sudo cp scripts/tv-webhook-trader.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable tv-webhook-trader
sudo systemctl start tv-webhook-trader

echo "=== 6. 방화벽 설정 ==="
sudo ufw allow 8443/tcp
sudo ufw --force enable

echo "=== 배포 완료! ==="
echo "TradingView Webhook URL: https://${DOMAIN}:8443/webhook"
echo "Health check: https://${DOMAIN}:8443/health"

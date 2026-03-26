# TradingView 웹훅 설정 가이드

## 1. Pine Script Alert 메시지 템플릿

TradingView 알람 생성 시 "Message" 란에 아래 JSON을 입력합니다.
`{{strategy.order.action}}`은 TV 내장 변수입니다.

### 기본 매수 알람
```json
{
  "token": "YOUR_SECRET_TOKEN_HERE",
  "action": "buy",
  "symbol": "BTCUSDT",
  "side": "open",
  "order_type": "market",
  "size": "0.01",
  "price": "0",
  "leverage": 10,
  "take_profit": "{{plot("TP")}}",
  "stop_loss": "{{plot("SL")}}",
  "comment": "{{strategy.order.comment}}"
}
```

### 기본 매도 알람
```json
{
  "token": "YOUR_SECRET_TOKEN_HERE",
  "action": "sell",
  "symbol": "BTCUSDT",
  "side": "open",
  "order_type": "market",
  "size": "0.01",
  "price": "0",
  "leverage": 10,
  "take_profit": "0",
  "stop_loss": "0",
  "comment": "{{strategy.order.comment}}"
}
```

### 포지션 청산 알람
```json
{
  "token": "YOUR_SECRET_TOKEN_HERE",
  "action": "close_long",
  "symbol": "BTCUSDT",
  "side": "close",
  "order_type": "market",
  "size": "0.01",
  "price": "0",
  "leverage": 10,
  "take_profit": "0",
  "stop_loss": "0",
  "comment": "Position close"
}
```

## 2. TradingView 알람 생성 방법

1. 차트에서 Pine Script 전략 적용
2. `알람(Alerts)` > `알람 추가(+)` 클릭
3. 조건: 전략의 주문 실행 조건 선택
4. 알림 방법: **Webhook URL** 체크
5. URL: `https://your-domain.com:8443/webhook`
6. Message: 위 JSON 템플릿 붙여넣기
7. 알람 이름 설정 후 저장

## 3. Pine Script에서 alert() 사용 예시

기존 Crypto Master Strategy에 webhook 알람 추가:

```pine
// 기존 전략 로직 끝부분에 추가
if (longCondition)
    strategy.entry("Long", strategy.long)
    alert('{"token":"YOUR_TOKEN","action":"buy","symbol":"' + syminfo.ticker + 'USDT","side":"open","order_type":"market","size":"0.01","price":"0","leverage":10,"take_profit":"0","stop_loss":"0","comment":"Long entry"}', alert.freq_once_per_bar_close)

if (shortCondition)
    strategy.entry("Short", strategy.short)
    alert('{"token":"YOUR_TOKEN","action":"sell","symbol":"' + syminfo.ticker + 'USDT","side":"open","order_type":"market","size":"0.01","price":"0","leverage":10,"take_profit":"0","stop_loss":"0","comment":"Short entry"}', alert.freq_once_per_bar_close)

if (exitLongCondition)
    strategy.close("Long")
    alert('{"token":"YOUR_TOKEN","action":"close_long","symbol":"' + syminfo.ticker + 'USDT","side":"close","order_type":"market","size":"0.01","price":"0","leverage":10,"take_profit":"0","stop_loss":"0","comment":"Exit long"}', alert.freq_once_per_bar_close)
```

## 4. 보안 체크리스트

- [ ] auth_token을 강력한 랜덤 문자열로 변경 (최소 32자)
- [ ] config/api_keys.json은 절대 git에 커밋하지 않기
- [ ] Bitget API 키는 IP 화이트리스트 + 선물만 허용으로 생성
- [ ] TradingView Webhook은 HTTPS만 지원 (SSL 인증서 필수)
- [ ] 서버 방화벽에서 8443 포트만 오픈
- [ ] 리스크 파라미터를 보수적으로 시작 (small size, tight loss limit)

## 5. 테스트 방법

서버 실행 후 curl로 테스트:
```bash
curl -k -X POST https://localhost:8443/webhook \
  -H "Content-Type: application/json" \
  -d '{
    "token": "YOUR_SECRET_TOKEN_HERE",
    "action": "buy",
    "symbol": "BTCUSDT",
    "side": "open",
    "order_type": "market",
    "size": "0.001",
    "price": "0",
    "leverage": 10,
    "take_profit": "0",
    "stop_loss": "0",
    "comment": "curl test"
  }'
```

Health check:
```bash
curl -k https://localhost:8443/health
```

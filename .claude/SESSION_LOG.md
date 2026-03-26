# TV Webhook Trader - Session Log
> 대화 압축 시 정보 누락 방지를 위한 누적 기록
> 마지막 업데이트: 2026-03-13 (멀티TF 비교 기능 추가)

---

## 1. 프로젝트 개요

| 항목 | 내용 |
|------|------|
| **프로젝트명** | tv-webhook-trader |
| **경로** | `C:\Users\gsh33\Projects\tv-webhook-trader` |
| **언어** | C++20 (MSVC 19.50, VS Community 2026) |
| **목적** | TradingView SFX 인디케이터 웹훅 → Bitget 선물 자동매매 |
| **관련 프로젝트** | `C:\Users\gsh33\Projects\sfx-trader` (Python 페이퍼 트레이딩 서버) |

### 빌드 환경
- **컴파일러**: Visual Studio 18 (2026 Community), MSVC 19.50
- **패키지 매니저**: vcpkg (`C:\Users\gsh33\vcpkg`)
- **의존성**: Boost 1.90, OpenSSL 3.6.1, spdlog, fmt, nlohmann-json (x64-windows)
- **빌드 경로**: `build/Release/`
- **바이너리**: `tv_webhook_trader.exe` (1.1MB), `backtest.exe` (219KB)
- **DLL**: `fmt.dll`, `libcrypto-3-x64.dll`, `libssl-3-x64.dll`, `spdlog.dll`

---

## 2. 아키텍처

```
TradingView Alert
    │ HTTPS POST (JSON)
    ▼
┌─────────────────────────────┐
│  HTTPS Webhook Server       │  (Boost.Beast + SSL)
│  - auth_token 인증          │
│  - IP whitelist (선택)      │
│  - JSON 파싱                │
└─────────┬───────────────────┘
          │ SignalData
          ▼
┌─────────────────────────────┐
│  Lock-Free Signal Queue     │  (SPSC or MPSC)
└─────────┬───────────────────┘
          │
          ▼
┌─────────────────────────────┐
│  Execution Engine           │
│  ┌───────────────────────┐  │
│  │ Worker Thread Pool    │  │  (num_workers=4)
│  │  ├ Worker 0           │  │
│  │  ├ Worker 1           │  │
│  │  ├ Worker 2           │  │
│  │  └ Worker 3           │  │
│  └───────────────────────┘  │
│  - RateLimiter (9 req/s)    │
│  - RiskManager 체크         │
│  - BitgetRestClient per-worker │
└─────────┬───────────────────┘
          │ REST API
          ▼
┌─────────────────────────────┐
│  Bitget V2 REST API         │
│  - 주문 (place_order)       │
│  - TP/SL (set_tpsl)         │
│  - 레버리지 (set_leverage)  │
└─────────────────────────────┘
```

---

## 3. 파일 구조 및 역할

### 헤더 파일 (include/)
| 파일 | 역할 | 비고 |
|------|------|------|
| `exchange/bitget_rest.hpp` | Bitget REST API 클라이언트 | V2 API, HMAC-SHA256 인증, place_order/set_tpsl/set_leverage |
| `server/http_server.hpp` | HTTPS 웹훅 서버 | Boost.Beast SSL, auth_token 검증 |
| `execution/execution_engine.hpp` | 멀티스레드 주문 실행 엔진 | Worker pool, RateLimiter, RiskManager 통합 |
| `core/signal_queue.hpp` | Lock-free 시그널 큐 | SPSC/MPSC |
| `core/types.hpp` | 공통 타입 정의 | SignalData, TradingConfig, AuthConfig 등 |
| `risk/risk_manager.hpp` | 리스크 관리 | max_daily_loss, max_drawdown, circuit_breaker |
| `backtest/log_parser.hpp` | sfx-trader state.json 파서 | BtSignal 구조체, filter_by_symbol, unique_symbols |
| `backtest/backtester.hpp` | 백테스트 시뮬레이션 엔진 | 부분 TP(33/33/34), SL, RE, 수수료 0.06% |
| `backtest/wf_optimizer.hpp` | Walk-Forward 최적화 | IS/OOS 롤링, 그리드 서치, Sharpe 목적함수 |

### 소스 파일 (src/)
| 파일 | 역할 |
|------|------|
| `main.cpp` | 메인 서버 진입점 (config 파싱, 서버+엔진 시작) |
| `backtest_main.cpp` | 백테스터 CLI (wf/grid/single 모드, --apply 플래그) |

### 설정 파일 (config/)
| 파일 | 역할 |
|------|------|
| `config.json` | 메인 설정 (웹훅, 트레이딩, 리스크) |
| `api_keys.json` | Bitget API 키 (api_key, api_secret, passphrase) - .gitignore에 포함 |

### 스크립트 (scripts/)
| 파일 | 역할 |
|------|------|
| `deploy.sh` | Ubuntu/Debian 배포 (apt, SSL, cmake, systemd) |
| `run_wf.sh` | WF 최적화 헬퍼 (--apply 옵션) |
| `preflight_check.sh` | 실거래 전 사전 점검 |

---

## 4. 주요 설정값 (config.json)

```json
{
    "webhook": {
        "port": 8443,
        "auth_token": "YOUR_SECRET_TOKEN_HERE"
    },
    "trading": {
        "trade_amount_usdt": 10.0,
        "default_leverage": 10,
        "num_workers": 4,
        "order_rate_limit": 9.0,
        "tpsl_rate_limit": 9.0,
        "symbols": ["224개 USDT 선물 심볼"]
    },
    "risk": {
        "max_position_per_symbol": 1.0,
        "max_daily_loss": 500.0,
        "max_open_orders": 250,
        "circuit_breaker_loss": 1000.0,
        "max_drawdown_pct": 5.0
    }
}
```

---

## 5. 현재 인프라 상태

| 항목 | 상태 | 상세 |
|------|------|------|
| sfx-trader (Python) | 실행 중 | localhost:5000 |
| ngrok 터널 | 활성 | `https://2d92-106-101-3-145.ngrok-free.app` → localhost:5000 |
| TradingView 웹훅 | 수신 중 | 425+ HTTP 요청, 426 연결 |
| 수집된 시그널 | 28개 | WF 최적화에 70개 이상 필요 |
| C++ 메인 서버 | 빌드 완료 | 아직 실행 안 함 (페이퍼 트레이딩 후) |
| C++ 백테스터 | 빌드 완료 | 28개 시그널로 테스트 완료 (grid fallback) |

---

## 6. 완료된 작업 이력

### 세션 1 (초기)
1. 프로젝트 구조 분석 및 최적화
2. 멀티스레드 실행 엔진 (Worker Pool) 구현
3. RateLimiter, RiskManager 통합
4. main.cpp v3 업데이트

### 세션 2 (빌드 ~ 배포 파이프라인)
1. 시스템 아키텍처 흐름도 설명
2. 거래 비중/레버리지 메커니즘 설명
3. **백테스터 + WF 최적화 구현** (log_parser, backtester, wf_optimizer, backtest_main)
4. **set_leverage() API 추가** (bitget_rest.hpp + init_leverage in execution_engine)
5. **7단계 프로덕션 파이프라인 완료**:
   - Step 1: 빌드 테스트 (vcpkg + MSVC) ✅
   - Step 2: 심볼 224개 확장 ✅
   - Step 3-4: deploy.sh + TradingView 가이드 ✅
   - Step 5: sfx-trader 페이퍼 트레이딩 (진행 중) ✅
   - Step 6: WF→config 자동 반영 (--apply) ✅
   - Step 7: preflight_check.sh ✅
6. API 키 설정 (config/api_keys.json)
7. ngrok vs VPS 비교 분석
8. 페이퍼 트레이딩 단계에서 ngrok 유지하기로 결정

### 세션 3 (2026-03-13)
- 대화 압축 후 이어서 시작
- 세션 로그 파일 생성 (본 파일)
- **멀티 타임프레임 비교 기능 구현**:
  - sfx-trader `server.py` v1.1.0 업데이트:
    - `Position` + `TradeRecord`에 `timeframe` 필드 추가
    - 시그널 저장 시 `timeframe` 필드 포함
    - 포지션 키를 `{symbol}_{timeframe}` 으로 변경 (멀티TF 동시 수집)
    - `get_stats()`에 `timeframe_stats` 섹션 추가
    - `_load()`에 하위 호환성 처리 (기존 state.json 마이그레이션)
    - `_pos_key()` 정적 메서드 추가
  - C++ `log_parser.hpp` 업데이트:
    - `BtSignal` + `BtTrade`에 `timeframe` 필드 추가
    - `filter_by_timeframe()`, `unique_timeframes()` 함수 추가
  - C++ `backtest_main.cpp` v1.1 업데이트:
    - `--timeframe` CLI 옵션 추가
    - `--mode compare` 추가 (타임프레임별 성능 비교 테이블)
    - `run_compare()` 함수: TF별 그리드 서치 → 최적 Sharpe 추천
  - 빌드 에러 수정: `avg_hold_minutes` → 제거 (BtMetrics에 미존재)

### 세션 4 (2026-03-13, 컨텍스트 연속)
- **TradingView Ultimate 플랜 결제 완료**
- 순차 테스트 → **5개 TF 동시 수집**으로 전략 변경
  - 1m, 5m, 15m, 60m, 240m Alert 동시 설정
- 1분봉 데이터 백업: `state_1m.json` (28개 시그널, 1건 거래)
- state.json 리셋 (잔고 1000 USDT, 멀티TF 동시 수집 시작)
- TradingView Alert 설정 가이드 제공 (각 TF별 `"timeframe"` 필드 추가)

---

## 7. 해결된 빌드 에러

| 에러 | 원인 | 해결 |
|------|------|------|
| `std::set` C2039 | `log_parser.hpp`에 `#include <set>` 누락 | `#include <set>` 추가 |
| C4244 port 변환 | int → uint16_t narrowing | `static_cast<uint16_t>()` |
| `_WIN32_WINNT` 미정의 | Boost.Asio 필요 | CMakeLists에 `_WIN32_WINNT=0x0A00` 추가 |
| C4100 unused param | `margin_mode` 미사용 | `/*margin_mode*/` 주석 처리 |

### 미해결 경고 (비치명적)
- C4324: struct padding in `types.hpp`
- C4100: unused `ec` in `http_server.hpp`

---

## 8. 다음 단계 (TODO)

- [x] **1분봉 데이터 백업**: `state_1m.json` (28개 시그널)
- [x] **TradingView Ultimate 플랜 결제**
- [x] **state.json 리셋** (멀티TF 동시 수집 시작)
- [ ] **TradingView Alert 5개 설정**: 1m, 5m, 15m, 60m, 240m (각각 `"timeframe"` 필드 포함)
- [ ] **sfx-trader 서버 재시작** (v1.1.0 멀티TF 적용)
- [ ] **1~2주 멀티TF 동시 데이터 수집**
- [ ] TF별 백테스트 비교: `backtest.exe state.json --mode compare`
- [ ] 심볼별 성능 분석 → 저성과 심볼 필터링
- [ ] 최적 TF 확정 후 실거래 파라미터 반영
- [ ] C++ 서버 테스트 (ngrok 연결)
- [ ] 안정화 확인 후 VPS 배포 (`scripts/deploy.sh`)

> **변경**: TradingView Ultimate 플랜으로 Alert 2000개 사용 가능 → 5개 TF 동시 수집

---

## 9. 사용자 의사결정 기록

| 일시 | 결정 | 내용 |
|------|------|------|
| 세션1 | 언어 선택 | C++20 (MSVC) |
| 세션2 | 백테스터 | C++ 내장, sfx-trader 로그 활용 |
| 세션2 | 최적화 대상 | position size + leverage |
| 세션2 | 심볼 소스 | 테스트_a0e67.txt (224개) |
| 세션2 | 페이퍼 단계 인프라 | ngrok 유지 (VPS 전환은 실거래 시) |
| 세션2 | 목적함수 | Sharpe Ratio (DD 페널티 포함) |
| 세션3 | 최적 분봉 탐색 | 순차 테스트 (15m → 60m), Alert 2개 제한 |
| 세션4 | Ultimate 플랜 | TradingView Ultimate 결제, 5TF 동시 수집으로 전환 |

---

## 10. 중요 경로 & 명령어

```bash
# 빌드
cd C:\Users\gsh33\Projects\tv-webhook-trader\build
cmake .. -DCMAKE_TOOLCHAIN_FILE=C:/Users/gsh33/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build . --config Release

# WF 최적화
build/Release/backtest.exe ../../sfx-trader/data/state.json --mode wf --apply config/config.json

# 타임프레임 비교
build/Release/backtest.exe ../../sfx-trader/data/state.json --mode compare

# 특정 TF만 WF 최적화
build/Release/backtest.exe ../../sfx-trader/data/state.json --mode wf --timeframe 15

# 메인 서버 실행
build/Release/tv_webhook_trader.exe config/config.json

# preflight 체크 (bash 환경)
bash scripts/preflight_check.sh
```

---

*이 파일은 대화가 진행될 때마다 업데이트됩니다.*

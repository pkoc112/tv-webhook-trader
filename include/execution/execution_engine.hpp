// ============================================================================
// execution/execution_engine.hpp -- 멀티스레드 주문 실행 엔진 v7.0
// 2026-03-16 | Refactored: PositionManager + TradeRecorder extracted
//
// v7.0 변경:
//   - PositionManager: 포지션 라이프사이클 관리 (position_manager.hpp)
//   - TradeRecorder: 거래 기록/통계/PnL 계산 (trade_recorder.hpp)
//   - ExecutionEngine: 시그널 처리 + 워커 + WebSocket 오케스트레이션
//
// v6.0 변경:
//   - 실제 PnL 계산 (entry/exit/qty 기반 + 수수료 차감)
//   - Default TP/SL 주입 (시그널 미제공 시 1.5%/1%)
//   - AlertManager 통합 (순환 버퍼 500개 + /api/alerts)
//   - WebSocket 포지션 콜백: 자동 제거 + PnL 기록
//   - TradeRecord 확장: entry_price, exit_price, quantity
// v5.0 변경:
//   - TP/SL 재시도 (3회 backoff) + 전체 실패 시 긴급 청산
//   - Preset TP/SL 제거 → set_sfx_tpsl() 단일 경로로 통합
//   - handle_tp/handle_sl 구현 (부분/전체 청산)
//   - 주문 성공 즉시 상태 저장
//   - StatePersistence fsync + corruption recovery
//
// 진입 파이프라인:
//   Signal → Tier체크 → Fee체크 → Portfolio리스크 → 동적사이징 → 주문실행
// ============================================================================
#pragma once

#include <thread>
#include <atomic>
#include <chrono>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <string>
#include <memory>
#include <spdlog/spdlog.h>

#include "core/types.hpp"
#include "core/spsc_queue.hpp"
#include "core/rate_limiter.hpp"
#include "core/state_persistence.hpp"
#include "webhook/signal_types.hpp"
#include "exchange/bitget_auth.hpp"
#include "exchange/bitget_rest.hpp"
#include "risk/risk_manager.hpp"
#include "risk/symbol_scorer.hpp"
#include "risk/fee_analyzer.hpp"
#include "risk/position_sizer.hpp"
#include "risk/portfolio_risk.hpp"
#include "exchange/bitget_ws.hpp"
#include "core/alert_manager.hpp"
#include "risk/symbol_learner.hpp"
#include "execution/position_manager.hpp"
#include "execution/trade_recorder.hpp"

namespace hft {

// -- TF 필터 설정 --
struct TfFilter {
    double size_multiplier{1.0};
    std::string min_tier{"C"};
    double min_tp_to_cost_ratio{1.5};
    bool   block_reverse{true};
};

struct TradingConfig {
    double default_size{0.01};
    double trade_amount_usdt{10.0};
    int    default_leverage{10};
    int    num_workers{4};
    double order_rate_limit{9.0};
    double tpsl_rate_limit{9.0};
    bool   shadow_mode{false};   // true = 로그만, 실주문 안함
    double backup_sl_pct{0.03};  // 백업 SL: TV SL 없을 때 3% 안전망
    std::unordered_set<std::string> allowed_symbols;
    std::unordered_map<std::string, double> symbol_sizes;
    std::unordered_map<std::string, int>    symbol_leverages;
    std::unordered_map<std::string, TfFilter> tf_filters;
};

struct EngineConfig {
    MPSCQueue<WebhookSignal>& signal_queue;
    BitgetAuth auth;
    BitgetRestConfig rest_config;
    RiskManager& risk_mgr;
    SymbolScorer& scorer;
    FeeAnalyzer& fee_analyzer;
    PositionSizer& sizer;
    PortfolioRiskManager& portfolio_risk;
    StatePersistence& state_store;
    AlertManager& alerts;
    SymbolLearner& learner;
    TradingConfig trading_config;
};

// ============================================================================
// Lock ordering contract (to prevent deadlocks):
//
//   1. SymbolLockManager (m_sym_locks) -- acquired FIRST via wait_lock()
//   2. m_pos_mtx (position/balance mutex) -- acquired AFTER symbol locks
//   3. PortfolioRiskManager::m_mtx -- acquired AFTER m_pos_mtx
//      (called while m_pos_mtx is held, e.g. check_entry, get_state)
//   4. SymbolScorer::m_mtx -- INDEPENDENT; never held simultaneously
//      with m_pos_mtx, so no ordering constraint
//   5. m_fp_mtx (fingerprint mutex) -- INDEPENDENT; never held with
//      m_pos_mtx or symbol locks
//
// Never acquire m_pos_mtx while holding PortfolioRiskManager::m_mtx.
// Never acquire symbol locks while holding m_pos_mtx.
// ============================================================================
class ExecutionEngine {
public:
    explicit ExecutionEngine(const EngineConfig& cfg)
        : m_signal_queue(cfg.signal_queue)
        , m_auth(cfg.auth)
        , m_rest_config(cfg.rest_config)
        , m_risk(cfg.risk_mgr)
        , m_scorer(cfg.scorer)
        , m_fee(cfg.fee_analyzer)
        , m_sizer(cfg.sizer)
        , m_port_risk(cfg.portfolio_risk)
        , m_state(cfg.state_store)
        , m_alerts(cfg.alerts)
        , m_learner(cfg.learner)
        , m_trading(cfg.trading_config)
        , m_order_limiter(m_trading.order_rate_limit)
        , m_tpsl_limiter(m_trading.tpsl_rate_limit)
        , m_pos_mgr(m_pos_mtx, m_balance, m_peak_balance, m_positions, m_port_risk, m_state)
        , m_trade_rec(m_pos_mtx, m_balance, m_peak_balance, m_trades, m_alerts, m_learner)
    {
        m_pos_mgr.set_trades_ref(&m_trades);
    }

    static constexpr int MAX_WORKERS = 32;

    void start() {
        // 상태 복구
        auto loaded = m_state.load_state();
        if (loaded.valid) {
            m_balance = loaded.balance;
            m_peak_balance = loaded.peak_balance;
            m_positions = std::move(loaded.positions);
            m_trades = std::move(loaded.trades);
            m_orders_executed.store(loaded.orders_executed, std::memory_order_relaxed);
            // m_next_oid를 타임스탬프 기반으로 재초기화 (재시작 시 clientOid 중복 방지)
            auto ts_oid = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count()) * 1000;
            m_next_oid.store(ts_oid, std::memory_order_relaxed);
            spdlog::info("[Exec] OID base set to {} (timestamp-based)", ts_oid);
            m_port_risk.update_balance(m_balance);

            if (!m_trades.empty()) {
                m_scorer.rescore_all(m_trades);
                // Bootstrap learner with historical trades
                for (const auto& t : m_trades) {
                    m_learner.record_trade(t);
                }
                spdlog::info("[Exec] Learner bootstrapped with {} historical trades", m_trades.size());
            }
            spdlog::info("[Exec] State restored: balance={:.2f} positions={} trades={}",
                m_balance, m_positions.size(), m_trades.size());
        }

        m_running.store(true, std::memory_order_relaxed);

        // Shadow/Live 모드 설정을 포트폴리오 리스크 매니저에 전파
        m_port_risk.set_shadow_mode(m_trading.shadow_mode);

        if (!m_trading.shadow_mode) {
            init_leverage();
        }

        int nw = std::min(std::max(1, m_trading.num_workers), MAX_WORKERS);
        auto now_epoch = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        for (int i = 0; i < nw; ++i) {
            m_worker_heartbeats[i].store(now_epoch, std::memory_order_relaxed);
        }
        m_num_workers_active = nw;
        for (int i = 0; i < nw; ++i) {
            m_workers.emplace_back([this, i] { worker_loop(i); });
        }

        // WebSocket 실시간 연결 시작
        if (!m_trading.shadow_mode) {
            start_websocket();
        }

        spdlog::info("[Exec] Engine started: {} workers | shadow={} | symbols: {}",
            nw, m_trading.shadow_mode, m_trading.allowed_symbols.size());
    }

    void stop() {
        m_running.store(false, std::memory_order_relaxed);
        m_signal_queue.shutdown();

        // WebSocket 종료
        if (m_ws_client) {
            m_ws_client->stop();
        }

        for (auto& t : m_workers) {
            if (t.joinable()) t.join();
        }

        // 최종 상태 저장
        {
            std::lock_guard lock(m_pos_mtx);
            m_pos_mgr.save_state(m_trades, m_orders_executed.load());
        }

        spdlog::info("[Exec] Stopped. Executed={} Rejected={} RiskSkip={} Skipped={}",
            m_orders_executed.load(), m_orders_rejected.load(),
            m_risk_skips.load(), m_orders_skipped.load());
    }

    // ── Accessors (thread-safe) ──

    [[nodiscard]] uint64_t orders_executed() const {
        return m_orders_executed.load(std::memory_order_relaxed);
    }
    [[nodiscard]] uint64_t risk_skip_count() const {
        return m_risk_skips.load(std::memory_order_relaxed);
    }
    [[nodiscard]] double balance() const {
        std::lock_guard lock(m_pos_mtx);
        return m_balance;
    }
    [[nodiscard]] double peak_balance() const {
        std::lock_guard lock(m_pos_mtx);
        return m_peak_balance;
    }

    [[nodiscard]] PortfolioState portfolio_state() const {
        std::lock_guard lock(m_pos_mtx);
        return m_port_risk.get_state(m_balance, m_positions, m_peak_balance);
    }

    [[nodiscard]] std::unordered_map<std::string, ManagedPosition> positions_snapshot() const {
        return m_pos_mgr.snapshot();
    }

    [[nodiscard]] nlohmann::json get_stats() const {
        // NOTE: 모든 공유 변수를 한 번의 lock으로 snapshot — deadlock + 불일치 방지
        // trade_rec.get_stats() 내부에서도 m_pos_mtx를 잡으므로, 여기서 전부 복사
        size_t pos_count;
        double balance_snap, peak_snap, equity_snap, upnl_snap;
        int total_trades;
        int wins = 0;
        double total_pnl = 0;
        {
            std::lock_guard lock(m_pos_mtx);
            pos_count = m_positions.size();
            balance_snap = m_balance;
            peak_snap = m_peak_balance;
            equity_snap = m_equity;
            upnl_snap = m_unrealized_pnl;
            total_trades = static_cast<int>(m_trades.size());
            for (auto& t : m_trades) {
                if (t.pnl > 0) wins++;
                total_pnl += t.pnl;
            }
        }
        // 이제 lock 없이 JSON 구성 (trade_rec.get_stats() 호출 안 함)
        nlohmann::json stats;
        stats["total_trades"] = total_trades;
        stats["wins"] = wins;
        stats["losses"] = total_trades - wins;
        stats["win_rate"] = total_trades > 0 ? std::round(static_cast<double>(wins) / total_trades * 10000.0) / 100.0 : 0.0;
        stats["total_pnl"] = std::round(total_pnl * 10000.0) / 10000.0;
        stats["balance"] = std::round(balance_snap * 100.0) / 100.0;
        stats["peak_balance"] = std::round(peak_snap * 100.0) / 100.0;
        stats["equity"] = std::round(equity_snap * 100.0) / 100.0;
        stats["unrealized_pnl"] = std::round(upnl_snap * 10000.0) / 10000.0;
        stats["open_positions"] = pos_count;
        stats["orders_executed"] = m_orders_executed.load();
        stats["risk_skips"] = m_risk_skips.load();
        stats["shadow_mode"] = m_trading.shadow_mode;
        stats["last_balance_sync"] = m_last_balance_sync.load(std::memory_order_relaxed);
        stats["ws_connected"] = m_ws_client ? true : false;
        return stats;
    }

    // Scorer access for dashboard
    SymbolScorer& scorer() { return m_scorer; }
    PortfolioRiskManager& portfolio_risk() { return m_port_risk; }
    AlertManager& alerts() { return m_alerts; }
    SymbolLearner& learner() { return m_learner; }

    [[nodiscard]] std::vector<TradeRecord> trades_snapshot() const {
        return m_trade_rec.snapshot();
    }

    // ── Strategy performance stats for identifying bad signals ──
    [[nodiscard]] nlohmann::json get_strategy_stats() const {
        return m_trade_rec.get_strategy_stats();
    }

private:
    void init_leverage() {
        net::io_context ioc;
        BitgetRestClient rest(ioc, m_auth, m_rest_config);

        // 실제 Bitget 잔고 조회 — available(가용잔고)로 m_balance 초기화
        auto bal_init = m_pos_mgr.fetch_real_balance(rest);
        if (bal_init.ok) {
            std::lock_guard lock(m_pos_mtx);
            if (bal_init.available > 0) {
                m_balance = bal_init.available;
            }
            if (bal_init.equity > 0) {
                m_equity = bal_init.equity;
            }
            m_unrealized_pnl = bal_init.unrealized_pnl;
            spdlog::info("[Exec] Init balance: available={:.2f} equity={:.2f} uPnL={:.2f}",
                m_balance, m_equity, m_unrealized_pnl);
        }

        // 거래소 포지션과 동기화 (ghost position 제거)
        m_pos_mgr.sync_positions_with_exchange(rest, m_orders_executed);

        // 심볼별 계약 정보 (sizeMultiplier) 로드
        rest.fetch_contracts();
        m_contracts = rest.get_contracts_cache();

        if (!m_trading.symbol_leverages.empty() || !m_trading.allowed_symbols.empty()) {
            for (auto& [symbol, lev] : m_trading.symbol_leverages) {
                m_order_limiter.acquire();
                rest.set_leverage(symbol, lev);
            }
            for (auto& symbol : m_trading.allowed_symbols) {
                if (m_trading.symbol_leverages.count(symbol)) continue;
                m_order_limiter.acquire();
                rest.set_leverage(symbol, m_trading.default_leverage);
            }
        }
        rest.disconnect();
        spdlog::info("[Exec] Leverage init complete, {} contracts loaded", m_contracts.size());
    }

    void start_websocket() {
        m_ws_client = std::make_unique<BitgetWSClient>(
            m_auth,
            // account 콜백 — 실시간 잔고 업데이트
            [this](const WsAccountUpdate& upd) {
                if (upd.available > 0) {
                    std::lock_guard lock(m_pos_mtx);
                    m_balance = upd.available;
                    m_equity = upd.equity;
                    m_unrealized_pnl = upd.unrealized_pnl;
                    if (upd.equity > m_peak_balance) m_peak_balance = upd.equity;
                    m_port_risk.update_balance(m_balance);
                }
                m_last_balance_sync.store(
                    static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count()),
                    std::memory_order_relaxed);
            },
            // position 콜백 — 포지션 청산 감지 → 내부 상태 동기화 + PnL
            [this](const WsPositionUpdate& upd) {
                if (upd.size <= 0) {
                    spdlog::info("[WS] Position closed: {} {}", upd.symbol, upd.hold_side);
                    std::lock_guard lock(m_pos_mtx);
                    auto result = m_pos_mgr.find_and_prepare_ws_close(upd.symbol, upd.hold_side);
                    if (result.found) {
                        double exit_price = upd.avg_price > 0 ? upd.avg_price : result.pos.entry_price;
                        double pnl = m_trade_rec.record_ws_close(result.pos, exit_price, upd.realized_pnl);
                        spdlog::info("[WS] Auto-removed {} {} PnL={:.4f}",
                            upd.symbol, upd.hold_side, pnl);
                        m_trade_rec.alert_trade("info", "WS close " + upd.symbol +
                            " PnL=" + std::to_string(pnl).substr(0,8), upd.symbol);
                        m_pos_mgr.save_state(m_trades, m_orders_executed.load());
                    }
                }
            },
            // order 콜백 — 체결 확인 로그
            [this](const WsOrderUpdate& upd) {
                if (upd.status == "full-fill" && upd.trade_side == "close") {
                    spdlog::info("[WS] Close filled: {} ${:.2f} fee={:.4f}",
                        upd.symbol, upd.filled_amount, upd.fee);
                }
            }
        );
        m_ws_client->start();
    }

    void worker_loop(int wid) {
        net::io_context ioc;
        BitgetRestClient rest(ioc, m_auth, m_rest_config);
        rest.set_contracts(m_contracts);
        spdlog::info("[Worker-{}] Started", wid);

        while (m_running.load(std::memory_order_relaxed)) {
            // Update heartbeat on every loop iteration (wake or signal)
            auto hb_now = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (wid < MAX_WORKERS)
                m_worker_heartbeats[wid].store(hb_now, std::memory_order_relaxed);

            auto sig_opt = m_signal_queue.wait_pop(std::chrono::milliseconds(500));
            if (!sig_opt) {
                if (wid == 0) periodic_tasks();
                continue;
            }
            try {
                process_signal(wid, rest, std::move(*sig_opt));
            } catch (const std::exception& e) {
                spdlog::error("[Worker-{}] EXCEPTION in process_signal: {}", wid, e.what());
            } catch (...) {
                spdlog::error("[Worker-{}] UNKNOWN EXCEPTION in process_signal", wid);
            }
        }
        rest.disconnect();
        spdlog::info("[Worker-{}] Stopped", wid);
    }

    void periodic_tasks() {
        if (m_risk.needs_daily_reset()) {
            m_risk.reset_daily();
            std::lock_guard lock(m_fp_mtx);
            m_recent_fingerprints.clear();
        }

        if (m_scorer.needs_rescore()) {
            std::lock_guard lock(m_pos_mtx);
            if (!m_trades.empty()) {
                m_scorer.rescore_all(m_trades);
            }
        }

        if (m_state.needs_save()) {
            std::lock_guard lock(m_pos_mtx);
            m_pos_mgr.save_state(m_trades, m_orders_executed.load());
        }

        // Periodic position sync with exchange (every 60 seconds)
        auto now_sec = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        auto last = m_last_sync_ts.load(std::memory_order_relaxed);
        constexpr int64_t SYNC_INTERVAL_SEC = 60;
        if (now_sec - last >= SYNC_INTERVAL_SEC) {
            m_last_sync_ts.store(now_sec, std::memory_order_relaxed);
            try {
                net::io_context sync_ioc;
                BitgetRestClient sync_rest(sync_ioc, m_auth, m_rest_config);
                sync_rest.set_contracts(m_contracts);
                m_pos_mgr.sync_positions_with_exchange(sync_rest, m_orders_executed);
                auto bal = m_pos_mgr.fetch_real_balance(sync_rest);
                if (bal.ok) {
                    std::lock_guard lock(m_pos_mtx);
                    m_equity = bal.equity;
                    m_unrealized_pnl = bal.unrealized_pnl;
                }

                // Update balance sync timestamp (epoch seconds)
                m_last_balance_sync.store(
                    static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(
                        std::chrono::system_clock::now().time_since_epoch()).count()),
                    std::memory_order_relaxed);

                spdlog::info("[Periodic] Position sync + balance refresh done (interval={}s)", SYNC_INTERVAL_SEC);
            } catch (const std::exception& e) {
                spdlog::warn("[Periodic] Sync failed: {}", e.what());
            }

            // Watchdog: check worker heartbeats
            constexpr int64_t WATCHDOG_TIMEOUT_SEC = 300;
            for (int i = 0; i < m_num_workers_active; ++i) {
                auto hb = m_worker_heartbeats[i].load(std::memory_order_relaxed);
                auto stale = now_sec - hb;
                if (stale > WATCHDOG_TIMEOUT_SEC) {
                    spdlog::critical("[WATCHDOG] Worker-{} appears stuck (no heartbeat for {}s)", i, stale);
                }
            }
        }
    }

    // Parse timeframe string (e.g. "1", "5", "15", "1h", "4h") to minutes
    static int parse_timeframe_minutes(const std::string& tf) {
        if (tf.empty()) return 0;
        try {
            if (tf.back() == 'h' || tf.back() == 'H') {
                return std::stoi(tf.substr(0, tf.size() - 1)) * 60;
            }
            if (tf.back() == 'd' || tf.back() == 'D') {
                return std::stoi(tf.substr(0, tf.size() - 1)) * 1440;
            }
            return std::stoi(tf);
        } catch (...) { return 0; }
    }

    void process_signal(int wid, BitgetRestClient& rest, WebhookSignal&& sig) {
        auto now = now_ns();
        auto latency_ns = now - sig.received_at;
        double latency_ms = latency_ns / 1'000'000.0;
        spdlog::info("[Worker-{}] {} {} {} @ {:.2f} strat={} (lat: {:.1f}ms)",
            wid, signal_type_str(sig.sig_type), sig.action, sig.symbol,
            sig.price, sig.strategy_name, latency_ms);

        // Stale signal detection - skip signals that are too old
        {
            int64_t sig_age_ms = static_cast<int64_t>(latency_ms);
            int tf_minutes = parse_timeframe_minutes(sig.timeframe);

            // TF-based staleness thresholds (ms)
            int64_t max_age_ms = 30000; // default 30s
            if (tf_minutes <= 1)       max_age_ms = 10000;   // 1m: 10s
            else if (tf_minutes <= 5)  max_age_ms = 30000;   // 5m: 30s
            else if (tf_minutes <= 15) max_age_ms = 60000;   // 15m: 60s
            else                       max_age_ms = 120000;  // 30m+: 120s

            if (sig_age_ms > max_age_ms) {
                spdlog::warn("[W-{}] STALE skip: {} {} age={}ms limit={}ms",
                    wid, sig.symbol, sig.action, sig_age_ms, max_age_ms);
                m_orders_skipped.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }

        // 중복 방지 (time-based eviction, 120s TTL)
        auto fp = sig.fingerprint();
        {
            std::lock_guard lock(m_fp_mtx);

            // Evict entries older than 120 seconds
            constexpr int64_t FP_TTL_NS = 120'000'000'000LL; // 120s in nanoseconds
            if (m_recent_fingerprints.size() > 100) {
                for (auto it = m_recent_fingerprints.begin(); it != m_recent_fingerprints.end(); ) {
                    if ((now - it->second) > FP_TTL_NS) {
                        it = m_recent_fingerprints.erase(it);
                    } else {
                        ++it;
                    }
                }
            }

            // Safety net: if map grows too large despite eviction, clear it
            if (m_recent_fingerprints.size() > 50000) {
                spdlog::warn("[W-{}] Fingerprint map overflow ({}), clearing", wid, m_recent_fingerprints.size());
                m_recent_fingerprints.clear();
            }

            if (m_recent_fingerprints.count(fp)) {
                spdlog::debug("[W-{}] SKIP {}: duplicate fingerprint", wid, sig.symbol);
                m_orders_skipped.fetch_add(1);
                return;
            }
            m_recent_fingerprints.emplace(fp, now);
        }

        // Re-validate signal after queue (defense in depth)
        if (!sig.is_valid()) {
            spdlog::warn("[W-{}] SKIP: invalid signal after dequeue: {} {} {}",
                wid, sig.symbol, sig.action, sig.alert);
            m_orders_skipped.fetch_add(1);
            return;
        }

        // 화이트리스트
        if (!m_trading.allowed_symbols.empty() &&
            !m_trading.allowed_symbols.count(sig.symbol)) {
            spdlog::debug("[W-{}] SKIP {}: not in allowed_symbols", wid, sig.symbol);
            m_orders_skipped.fetch_add(1);
            return;
        }

        switch (sig.sig_type) {
            case SignalType::Entry:
            case SignalType::ReEntry:
                handle_entry(wid, rest, std::move(sig));
                break;
            case SignalType::TP:
                handle_tp(wid, rest, sig);
                break;
            case SignalType::SL:
                handle_sl(wid, rest, sig);
                break;
            default:
                spdlog::warn("[Worker-{}] Unknown: {}", wid, sig.alert);
                break;
        }
    }

    // ── 진입: 고급 리스크 파이프라인 ──
    void handle_entry(int wid, BitgetRestClient& rest, WebhookSignal&& sig) {
        if (!m_sym_locks.wait_lock(sig.symbol, std::chrono::seconds(15))) {
            spdlog::warn("[W-{}] SKIP {}: symbol lock timeout (15s)", wid, sig.symbol);
            m_orders_skipped.fetch_add(1);
            return;
        }
        SymbolLockGuard guard(m_sym_locks, sig.symbol);

        std::string tf = sig.timeframe.empty() ? "unknown" : sig.timeframe;

        // 0. USDC 심볼 필터 (현재 USDT-FUTURES만 지원)
        if (sig.symbol.size() > 4 && sig.symbol.substr(sig.symbol.size() - 4) == "USDC") {
            spdlog::info("[W-{}] SKIP {}: USDC symbol not supported (USDT-FUTURES only)", wid, sig.symbol);
            m_orders_skipped.fetch_add(1);
            return;
        }

        // 1. 티어 체크 — Shadow vs Live 모드 분기
        std::string tier = m_scorer.get_tier(sig.symbol);
        if (m_trading.shadow_mode) {
            // Shadow 모드: 모든 티어 허용 (학습 데이터 수집)
            // X/D 티어도 진입 → 거래 기록 → 스코어링에 반영
            spdlog::debug("[W-{}] SHADOW {} tier={} (all tiers allowed)", wid, sig.symbol, tier);
        } else {
            // Live 모드: 학습 검증된 심볼만 실전 매매
            std::string live_min = m_port_risk.get_live_min_tier();
            if (!tier_meets_min(tier, live_min)) {
                spdlog::info("[W-{}] SKIP {}: tier {} < live_min {} (unproven symbol)",
                    wid, sig.symbol, tier, live_min);
                m_risk_skips.fetch_add(1);
                return;
            }
        }

        // 1.5. 학습 엔진 판단 (L1~L4)
        int current_hour_utc = -1;
        {
            auto now_t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            std::tm tm{};
#ifdef _WIN32
            gmtime_s(&tm, &now_t);
#else
            gmtime_r(&now_t, &tm);
#endif
            current_hour_utc = tm.tm_hour;
        }
        auto learn_dec = m_learner.evaluate(sig.symbol, tf, current_hour_utc);
        if (!learn_dec.allowed) {
            spdlog::info("[W-{}] LEARN_SKIP {}: {}", wid, sig.symbol, learn_dec.skip_reason);
            m_alerts.warn("RISK", "LEARN_SKIP " + sig.symbol + " " + learn_dec.skip_reason, sig.symbol);
            m_risk_skips.fetch_add(1);
            return;
        }

        // 2. TF 필터 (shadow 모드에서는 티어 체크 스킵, reverse 블록만 유지)
        auto tf_it = m_trading.tf_filters.find(tf);
        if (tf_it != m_trading.tf_filters.end()) {
            auto& filt = tf_it->second;
            if (filt.block_reverse && sig.sig_type == SignalType::ReEntry) {
                spdlog::info("[W-{}] SKIP {}: ReEntry blocked for TF {}", wid, sig.symbol, tf);
                m_risk_skips.fetch_add(1);
                return;
            }
            if (!m_trading.shadow_mode && !tier_meets_min(tier, filt.min_tier)) {
                spdlog::info("[W-{}] SKIP {}: tier {} < {} for TF {}",
                    wid, sig.symbol, tier, filt.min_tier, tf);
                m_risk_skips.fetch_add(1);
                return;
            }
        }

        // 3. 수수료 수익성 (shadow 모드에서는 로그만, 차단 안 함)
        if (sig.has_tp1() && sig.price > 0) {
            double custom_ratio = 0;
            if (tf_it != m_trading.tf_filters.end())
                custom_ratio = tf_it->second.min_tp_to_cost_ratio;
            auto tp_chk = m_fee.is_tp_profitable(sig.symbol, sig.price, sig.tp1, custom_ratio);
            if (!tp_chk.profitable) {
                if (m_trading.shadow_mode) {
                    // Shadow: 기록은 하되 차단하지 않음 (수수료 부족 시그널도 학습)
                    spdlog::debug("[W-{}] SHADOW_FEE {}: ratio={:.1f} < {:.1f} (allowed for learning)",
                        wid, sig.symbol, tp_chk.ratio, tp_chk.min_ratio);
                } else {
                    spdlog::info("[W-{}] SKIP {}: TP1 ratio={:.1f} < {:.1f}",
                        wid, sig.symbol, tp_chk.ratio, tp_chk.min_ratio);
                    m_risk_skips.fetch_add(1);
                    return;
                }
            }
        }

        // 4. 동적 사이징 (가용 마진 기반)
        double balance_now, peak_now, dd_pct, used_margin;
        {
            std::lock_guard lock(m_pos_mtx);
            balance_now = m_balance;
            peak_now = m_peak_balance;
            dd_pct = peak_now > 0 ? (peak_now - balance_now) / peak_now * 100.0 : 0;
            used_margin = m_pos_mgr.calc_used_margin(m_trading.default_leverage);
        }

        auto score = m_scorer.get_score(sig.symbol);
        int leverage = m_trading.default_leverage;
        if (score) leverage = std::min(leverage, score->max_leverage);

        double sl_price = sig.has_sl() ? sig.sl : 0;
        auto sz = m_sizer.calc_size(balance_now, sig.symbol, sig.price, sl_price, leverage, score, dd_pct, used_margin);

        if (sz.usdt_amount <= 0 || sz.qty <= 0) {
            spdlog::info("[W-{}] SKIP {}: size=0 ({})", wid, sig.symbol, sz.reason);
            m_risk_skips.fetch_add(1);
            return;
        }

        // TF 사이즈 배수
        if (tf_it != m_trading.tf_filters.end()) {
            sz.qty *= tf_it->second.size_multiplier;
            sz.usdt_amount *= tf_it->second.size_multiplier;
        }

        // L4: 학습 기반 사이즈 배율 적용
        if (std::abs(learn_dec.size_multiplier - 1.0) > 0.01) {
            sz.qty *= learn_dec.size_multiplier;
            sz.usdt_amount *= learn_dec.size_multiplier;
            spdlog::info("[W-{}] LEARN_SIZE {}: x{:.2f} q={:.1f}%",
                wid, sig.symbol, learn_dec.size_multiplier, learn_dec.signal_quality * 100.0);
        }

        // 거래소 최소 주문금액 보장 (Bitget 5 USDT)
        constexpr double MIN_NOTIONAL_USDT = 5.5;  // 5 USDT + 마진
        if (sig.price > 0 && sz.usdt_amount < MIN_NOTIONAL_USDT) {
            sz.qty = MIN_NOTIONAL_USDT / sig.price;
            sz.usdt_amount = MIN_NOTIONAL_USDT;
        }

        sig.size = sz.qty;
        leverage = sz.leverage;

        // 5. 포트폴리오 리스크
        {
            std::lock_guard lock(m_pos_mtx);
            std::string entry_side = sig.action == "buy" ? "long" : "short";

            // 5a. Opposite direction conflict: block if same symbol has opposite position
            std::string opp_side = (entry_side == "long") ? "short" : "long";
            for (auto& [_, p] : m_positions) {
                if (p.symbol == sig.symbol && p.side == opp_side) {
                    spdlog::warn("[W-{}] CONFLICT {}: existing {} vs new {} — SKIP",
                        wid, sig.symbol, opp_side, entry_side);
                    m_risk_skips.fetch_add(1);
                    return;
                }
            }

            // 5b. Same symbol same direction: block duplicate entry
            for (auto& [_, p] : m_positions) {
                if (p.symbol == sig.symbol && p.side == entry_side) {
                    spdlog::info("[W-{}] SKIP {}: already has {} position", wid, sig.symbol, entry_side);
                    m_orders_skipped.fetch_add(1);
                    return;
                }
            }

            auto dec = m_port_risk.check_entry(
                sig.symbol, tf, sig.price, sig.size, leverage, m_balance, m_positions,
                sl_price, entry_side);
            if (!dec.allowed) {
                spdlog::info("[W-{}] RISK {}: {}", wid, sig.symbol, dec.reason);
                m_risk_skips.fetch_add(1);
                return;
            }
        }

        // 6. 주문
        auto oid = m_next_oid.fetch_add(1);
        auto order_req = sig.to_order_request(oid);

        if (!m_risk.validate(order_req)) {
            spdlog::warn("[W-{}] SKIP {}: risk_manager.validate() rejected", wid, sig.symbol);
            m_orders_rejected.fetch_add(1);
            return;
        }

        spdlog::info("[W-{}] ENTRY {} {} sz={:.6f} lev={}x ${:.2f} tier={} | {}",
            wid, sig.action, sig.symbol, sig.size, leverage, sz.usdt_amount, tier, sz.reason);

        std::string side_str = sig.action == "buy" ? "long" : "short";

        if (m_trading.shadow_mode) {
            spdlog::info("[W-{}] SHADOW {} {} sz={:.6f}", wid, sig.action, sig.symbol, sig.size);
            m_orders_executed.fetch_add(1);
            m_pos_mgr.register_position(sig.symbol, tf, side_str,
                sig.price, sig.size, leverage, sl_price,
                sig.has_tp1() ? sig.tp1 : 0, tier, oid, sig.strategy_name);
            return;
        }

        // 주문 실행 (preset TP/SL 포함 — 별도 API보다 안정적)
        double preset_tp = sig.has_tp1() ? sig.tp1 : 0;
        double preset_sl = sig.has_sl() ? sig.sl : 0;

        // 백업 SL: TV에서 SL 미제공 시 안전망 (기본 3%)
        // TV SL 시그널이 1차, 이 프리셋이 2차 안전장치
        if (preset_sl <= 0 && m_trading.backup_sl_pct > 0 && sig.price > 0) {
            bool is_long = (sig.action == "buy");
            preset_sl = is_long
                ? sig.price * (1.0 - m_trading.backup_sl_pct)
                : sig.price * (1.0 + m_trading.backup_sl_pct);
            spdlog::info("[W-{}] Backup SL injected: {:.4f} ({:.1f}% from entry {:.4f})",
                wid, preset_sl, m_trading.backup_sl_pct * 100.0, sig.price);
        }
        m_order_limiter.acquire();
        m_risk.on_order_placed();
        auto resp = rest.place_futures_order(order_req, preset_tp, preset_sl);
        m_risk.on_order_done();

        if (resp.status == OrderStatus::New) {
            m_orders_executed.fetch_add(1);
            m_risk.on_position_opened(sig.symbol, sig.size, side_str);
            m_pos_mgr.register_position(sig.symbol, tf, side_str,
                sig.price, sig.size, leverage, sl_price,
                sig.has_tp1() ? sig.tp1 : 0, tier, oid, sig.strategy_name);

            spdlog::info("[W-{}] OK {} {} sz={:.6f} exid={}",
                wid, sig.action, sig.symbol, sig.size, resp.exchange_order_id);
            std::string entry_type = (sig.sig_type == SignalType::ReEntry) ? "REENTRY" : "ENTRY";
            spdlog::info("[TRADE] {} {} {} {}x @ {:.4f} ${:.2f} TP1={:.4f} SL={:.4f}",
                entry_type, (sig.action == "buy" ? "LONG" : "SHORT"), sig.symbol, leverage,
                sig.price, sz.usdt_amount, sig.tp1, sig.sl);
            m_alerts.info("TRADE", entry_type + " " + sig.action + " " + sig.symbol +
                " sz=" + std::to_string(sig.size).substr(0,8) + " lev=" + std::to_string(leverage) + "x",
                sig.symbol);

            // Fix 4: 주문 성공 즉시 상태 저장 (크래시 시 포지션 유실 방지)
            {
                std::lock_guard lock(m_pos_mtx);
                m_pos_mgr.save_state(m_trades, m_orders_executed.load());
            }

            // TP/SL: preset으로 주문에 포함됨. 별도 API 호출 불필요.
            if (preset_tp > 0 || preset_sl > 0) {
                spdlog::info("[W-{}] Preset TP/SL included in order: TP={:.4f} SL={:.4f}",
                    wid, preset_tp, preset_sl);
            } else {
                spdlog::info("[W-{}] No TP/SL in signal — relying on TV TP/SL signals", wid);
            }
        } else {
            m_orders_rejected.fetch_add(1);
            spdlog::error("[W-{}] FAIL: err={} {}", wid, resp.error_code,
                std::string(resp.error_msg.data()));
        }
    }

    // ── TP 시그널 처리: 부분/전체 청산 ──
    void handle_tp(int wid, BitgetRestClient& rest, const WebhookSignal& sig) {
        spdlog::info("[W-{}] TP {} {} @ {:.2f}", wid, sig.tp_level, sig.symbol, sig.price);

        // Symbol lock to prevent race with entry/SL on same symbol
        if (!m_sym_locks.wait_lock(sig.symbol, std::chrono::seconds(10))) {
            spdlog::warn("[W-{}] SKIP TP {}: symbol lock timeout", wid, sig.symbol);
            return;
        }
        SymbolLockGuard guard(m_sym_locks, sig.symbol);

        // hold_side 결정: bull 시그널 = long 포지션 청산
        std::string hold_side = (sig.signal_direction == "bull") ? "long" : "short";

        // 매칭되는 포지션 찾기
        auto match = m_pos_mgr.find_by_symbol_side(sig.symbol, hold_side);

        if (match.key.empty()) {
            spdlog::warn("[W-{}] TP {} no matching position for {} hold={}",
                wid, sig.tp_level, sig.symbol, hold_side);
            return;
        }

        // TP 레벨별 청산 비율: TP1=33%, TP2=33%, TP3=나머지 전부
        double close_ratio = 1.0;
        if (sig.tp_level == "TP1")      close_ratio = 0.33;
        else if (sig.tp_level == "TP2") close_ratio = 0.50;  // 남은 67%의 50% ≈ 33%

        double close_qty = match.quantity * close_ratio;

        if (m_trading.shadow_mode) {
            // Shadow mode: simulate close without touching exchange
            spdlog::info("[SHADOW] TP {} {} close_qty={:.6f}/{:.6f} hold={}",
                sig.tp_level, sig.symbol, close_qty, match.quantity, hold_side);

            std::lock_guard lock(m_pos_mtx);
            auto* pos = m_pos_mgr.get(match.key);
            if (pos) {
                double exit_price = sig.price > 0 ? sig.price : pos->entry_price;

                if (sig.tp_level == "TP3" || close_ratio >= 0.99) {
                    double pnl = m_trade_rec.record_close(*pos, exit_price, sig.tp_level, close_qty);
                    m_pos_mgr.remove(match.key);
                    spdlog::info("[SHADOW] {} {} PnL={:.4f} @ {:.4f} (100%)",
                        sig.tp_level, sig.symbol, pnl, exit_price);
                    m_trade_rec.alert_trade("info", "[SHADOW] " + sig.tp_level + " " + sig.symbol +
                        " PnL=" + std::to_string(pnl).substr(0,8), sig.symbol);
                } else {
                    double pnl = m_trade_rec.record_close(*pos, exit_price,
                        sig.tp_level + "_PARTIAL", close_qty);
                    m_pos_mgr.reduce_quantity(match.key, close_qty);
                    spdlog::info("[SHADOW] {} {} PnL={:.4f} @ {:.4f} ({:.0f}%)",
                        sig.tp_level, sig.symbol, pnl, exit_price, close_ratio * 100);
                }
                m_pos_mgr.save_state(m_trades, m_orders_executed.load());
            }
            return;
        }

        // Retry logic for TP close (transient failures)
        constexpr int MAX_CLOSE_RETRIES = 3;
        OrderResponse resp;
        for (int attempt = 1; attempt <= MAX_CLOSE_RETRIES; ++attempt) {
            m_order_limiter.acquire();
            resp = rest.close_partial(sig.symbol, close_qty, hold_side);
            if (resp.status == OrderStatus::New) break;
            if (attempt < MAX_CLOSE_RETRIES) {
                spdlog::warn("[W-{}] TP {} close retry {}/{}: {} {}",
                    wid, sig.tp_level, attempt, MAX_CLOSE_RETRIES,
                    resp.error_code, std::string(resp.error_msg.data()));
                std::this_thread::sleep_for(std::chrono::milliseconds(200 * attempt));
            }
        }

        if (resp.status == OrderStatus::New) {
            spdlog::info("[W-{}] TP {} closed {:.6f}/{:.6f} of {}",
                wid, sig.tp_level, close_qty, match.quantity, sig.symbol);

            std::lock_guard lock(m_pos_mtx);
            auto* pos = m_pos_mgr.get(match.key);
            if (pos) {
                double exit_price = sig.price > 0 ? sig.price : pos->entry_price;

                if (sig.tp_level == "TP3" || close_ratio >= 0.99) {
                    // 전체 청산 → 거래 기록 + 포지션 제거
                    double pnl = m_trade_rec.record_close(*pos, exit_price, sig.tp_level, close_qty);
                    m_pos_mgr.remove(match.key);
                    spdlog::info("[TRADE] {} {} {:.4f} @ {:.4f} (100%)",
                        sig.tp_level, sig.symbol, pnl, exit_price);
                    m_trade_rec.alert_trade("info", sig.tp_level + " " + sig.symbol +
                        " PnL=" + std::to_string(pnl).substr(0,8), sig.symbol);
                } else {
                    // 부분 청산 → 수량 차감 + PnL 기록
                    double pnl = m_trade_rec.record_close(*pos, exit_price,
                        sig.tp_level + "_PARTIAL", close_qty);
                    m_pos_mgr.reduce_quantity(match.key, close_qty);
                    spdlog::info("[TRADE] {} {} {:.4f} @ {:.4f} ({:.0f}%)",
                        sig.tp_level, sig.symbol, pnl, exit_price, close_ratio * 100);
                }
                m_pos_mgr.save_state(m_trades, m_orders_executed.load());
            }
        } else {
            spdlog::error("[W-{}] TP {} close FAILED after {} retries: {} {}",
                wid, sig.tp_level, MAX_CLOSE_RETRIES, resp.error_code,
                std::string(resp.error_msg.data()));
            m_alerts.warn("TRADE", "TP close failed " + sig.tp_level + " " + sig.symbol, sig.symbol);
        }
    }

    // ── SL 시그널 처리: 전체 청산 ──
    void handle_sl(int wid, BitgetRestClient& rest, const WebhookSignal& sig) {
        spdlog::info("[W-{}] SL {} @ {:.2f}", wid, sig.symbol, sig.price);

        // Symbol lock to prevent race with entry/TP on same symbol
        if (!m_sym_locks.wait_lock(sig.symbol, std::chrono::seconds(10))) {
            spdlog::warn("[W-{}] SKIP SL {}: symbol lock timeout", wid, sig.symbol);
            return;
        }
        SymbolLockGuard guard(m_sym_locks, sig.symbol);

        std::string hold_side = (sig.signal_direction == "bull") ? "long" : "short";

        // 매칭 포지션 찾기
        auto match = m_pos_mgr.find_by_symbol_side(sig.symbol, hold_side);

        if (match.key.empty()) {
            spdlog::warn("[W-{}] SL no matching position for {} hold={}",
                wid, sig.symbol, hold_side);
            return;
        }

        if (m_trading.shadow_mode) {
            // Shadow mode: simulate full close without touching exchange
            spdlog::info("[SHADOW] SL {} hold={} qty={:.6f}", sig.symbol, hold_side, match.quantity);

            std::lock_guard lock(m_pos_mtx);
            auto* pos = m_pos_mgr.get(match.key);
            if (pos) {
                double exit_price = sig.price > 0 ? sig.price : pos->sl_price;
                double pnl = m_trade_rec.record_close(*pos, exit_price, "SL");
                m_pos_mgr.remove(match.key);
                m_pos_mgr.save_state(m_trades, m_orders_executed.load());
                spdlog::info("[SHADOW] SL {} PnL={:.4f} @ {:.4f}",
                    sig.symbol, pnl, exit_price);
                m_trade_rec.alert_trade("warn", "[SHADOW] SL " + sig.symbol +
                    " PnL=" + std::to_string(pnl).substr(0,8), sig.symbol);
            }
            return;
        }

        // flash close with retry (SL must succeed)
        constexpr int MAX_SL_RETRIES = 3;
        bool closed = false;
        for (int attempt = 1; attempt <= MAX_SL_RETRIES; ++attempt) {
            m_order_limiter.acquire();
            closed = rest.flash_close_position(sig.symbol, hold_side);
            if (closed) break;
            if (attempt < MAX_SL_RETRIES) {
                spdlog::warn("[W-{}] SL close retry {}/{} for {} hold={}",
                    wid, attempt, MAX_SL_RETRIES, sig.symbol, hold_side);
                std::this_thread::sleep_for(std::chrono::milliseconds(300 * attempt));
            }
        }

        if (closed) {
            std::lock_guard lock(m_pos_mtx);
            auto* pos = m_pos_mgr.get(match.key);
            if (pos) {
                double exit_price = sig.price > 0 ? sig.price : pos->sl_price;
                double pnl = m_trade_rec.record_close(*pos, exit_price, "SL");
                m_pos_mgr.remove(match.key);
                m_pos_mgr.save_state(m_trades, m_orders_executed.load());
                spdlog::info("[TRADE] SL {} {:.4f} @ {:.4f}",
                    sig.symbol, pnl, exit_price);
                m_trade_rec.alert_trade("warn", "SL " + sig.symbol +
                    " PnL=" + std::to_string(pnl).substr(0,8), sig.symbol);
            } else {
                // Position already removed by WS callback — that's OK, WS recorded PnL
                spdlog::info("[W-{}] SL {} position already removed (WS callback)", wid, sig.symbol);
            }
            spdlog::info("[W-{}] SL closed {} hold={}", wid, sig.symbol, hold_side);
        } else {
            spdlog::error("[W-{}] SL close FAILED after {} retries for {} hold={} — ALERT",
                wid, MAX_SL_RETRIES, sig.symbol, hold_side);
            m_alerts.critical("TRADE", "SL CLOSE FAILED " + sig.symbol + " " + hold_side +
                " — manual intervention needed!", sig.symbol);
        }
    }

    // ── TP/SL 설정: 3회 재시도 + 실패 시 긴급 청산 ──
    void set_sfx_tpsl(int wid, BitgetRestClient& rest, const WebhookSignal& sig) {
        std::string hold_side = sig.get_hold_side();
        bool tp_ok = !sig.has_tp1();  // TP 없으면 성공으로 간주
        bool sl_ok = !sig.has_sl();   // SL 없으면 성공으로 간주

        constexpr int MAX_RETRIES = 3;
        constexpr int BASE_DELAY_MS = 200;  // 200ms, 400ms, 800ms

        // TP 재시도
        if (sig.has_tp1()) {
            for (int attempt = 1; attempt <= MAX_RETRIES; ++attempt) {
                try {
                    m_tpsl_limiter.acquire();
                    tp_ok = rest.place_tpsl(sig.symbol, "profit_plan", sig.tp1, sig.size, hold_side);
                    if (tp_ok) break;
                } catch (const std::exception& e) {
                    spdlog::warn("[W-{}] TP attempt {}/{} failed: {}", wid, attempt, MAX_RETRIES, e.what());
                }
                if (attempt < MAX_RETRIES) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(BASE_DELAY_MS * (1 << (attempt - 1))));
                }
            }
        }

        // SL 재시도
        if (sig.has_sl()) {
            for (int attempt = 1; attempt <= MAX_RETRIES; ++attempt) {
                try {
                    m_tpsl_limiter.acquire();
                    sl_ok = rest.place_tpsl(sig.symbol, "loss_plan", sig.sl, sig.size, hold_side);
                    if (sl_ok) break;
                } catch (const std::exception& e) {
                    spdlog::warn("[W-{}] SL attempt {}/{} failed: {}", wid, attempt, MAX_RETRIES, e.what());
                }
                if (attempt < MAX_RETRIES) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(BASE_DELAY_MS * (1 << (attempt - 1))));
                }
            }
        }

        // SL 설정 실패 → 보호 없는 포지션은 즉시 긴급 청산
        if (!sl_ok) {
            spdlog::error("[W-{}] CRITICAL: SL failed after {} retries for {} — EMERGENCY CLOSE",
                wid, MAX_RETRIES, sig.symbol);

            // Emergency close with retry and result verification
            bool emergency_closed = false;
            for (int ec_attempt = 1; ec_attempt <= 3; ++ec_attempt) {
                m_order_limiter.acquire();
                emergency_closed = rest.flash_close_position(sig.symbol, hold_side);
                if (emergency_closed) break;
                spdlog::error("[W-{}] Emergency close attempt {}/3 FAILED for {}", wid, ec_attempt, sig.symbol);
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }

            if (!emergency_closed) {
                spdlog::error("[W-{}] ALL EMERGENCY CLOSE ATTEMPTS FAILED for {} — UNPROTECTED POSITION!",
                    wid, sig.symbol);
                m_alerts.critical("EMERGENCY",
                    "UNPROTECTED POSITION " + sig.symbol + " " + hold_side +
                    " — ALL close attempts failed! Manual intervention required!", sig.symbol);
            }

            // 내부 상태에서도 포지션 제거 (only if close succeeded)
            if (emergency_closed) {
                std::lock_guard lock(m_pos_mtx);
                auto result = m_pos_mgr.find_and_prepare_ws_close(sig.symbol, hold_side);
                if (result.found) {
                    double exit_price = sig.price;  // 긴급청산은 현재가
                    double pnl = m_trade_rec.record_close(result.pos, exit_price, "EMERGENCY_CLOSE_NO_SL");
                    m_trade_rec.alert_trade("critical", "EMERGENCY CLOSE " + sig.symbol +
                        " PnL=" + std::to_string(pnl).substr(0,8), sig.symbol);
                }
                m_pos_mgr.save_state(m_trades, m_orders_executed.load());
            }
        } else if (!tp_ok) {
            // TP만 실패: 위험하지 않으므로 로그만 (SL은 있음)
            spdlog::warn("[W-{}] TP failed after {} retries for {} — SL is active, continuing",
                wid, MAX_RETRIES, sig.symbol);
        }
    }

    static bool tier_meets_min(const std::string& tier, const std::string& min_tier) {
        static const std::unordered_map<std::string, int> rank =
            {{"S",5},{"A",4},{"B",3},{"C",2},{"D",1},{"X",0}};
        auto t = rank.find(tier);
        auto m = rank.find(min_tier);
        int tv = t != rank.end() ? t->second : 2;
        int mv = m != rank.end() ? m->second : 2;
        return tv >= mv;
    }

    // ── Shared state ──
    MPSCQueue<WebhookSignal>& m_signal_queue;
    BitgetAuth m_auth;
    BitgetRestConfig m_rest_config;
    std::unordered_map<std::string, BitgetRestClient::ContractInfo> m_contracts;
    RiskManager& m_risk;
    SymbolScorer& m_scorer;
    FeeAnalyzer& m_fee;
    PositionSizer& m_sizer;
    PortfolioRiskManager& m_port_risk;
    StatePersistence& m_state;
    AlertManager& m_alerts;
    SymbolLearner& m_learner;
    TradingConfig m_trading;

    RateLimiter m_order_limiter;
    RateLimiter m_tpsl_limiter;
    SymbolLockManager m_sym_locks;

    std::vector<std::thread> m_workers;
    std::atomic<bool> m_running{false};
    std::atomic<uint64_t> m_next_oid{
        static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count()) * 1000
    };

    std::atomic<uint64_t> m_orders_executed{0};
    std::atomic<uint64_t> m_orders_rejected{0};
    std::atomic<uint64_t> m_orders_skipped{0};
    std::atomic<uint64_t> m_risk_skips{0};

    std::mutex m_fp_mtx;
    std::unordered_map<size_t, int64_t> m_recent_fingerprints;  // fingerprint -> timestamp_ns

    std::atomic<int64_t> m_last_sync_ts{0};  // epoch seconds of last position sync
    std::atomic<int64_t> m_last_balance_sync{0};  // epoch seconds of last balance sync

    // Worker heartbeat watchdog
    std::atomic<int64_t> m_worker_heartbeats[MAX_WORKERS]{};
    int m_num_workers_active{0};

    mutable std::mutex m_pos_mtx;
    double m_balance{100.0};   // 안전 기본값; start() 시 Bitget 실잔고로 덮어씀
    double m_peak_balance{100.0};
    double m_equity{0.0};          // 총 자산 (available + margin + unrealized PnL)
    double m_unrealized_pnl{0.0};  // 미실현 PnL
    std::unordered_map<std::string, ManagedPosition> m_positions;
    std::vector<TradeRecord> m_trades;

    // ── Delegated components ──
    PositionManager m_pos_mgr;
    TradeRecorder m_trade_rec;

    // WebSocket 실시간 클라이언트
    std::unique_ptr<BitgetWSClient> m_ws_client;
};

} // namespace hft

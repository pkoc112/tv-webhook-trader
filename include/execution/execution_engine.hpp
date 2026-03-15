// ============================================================================
// execution/execution_engine.hpp -- 멀티스레드 주문 실행 엔진 v6.0
// 2026-03-15 | A-grade 개선 패치
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
    std::unordered_set<std::string> allowed_symbols;
    std::unordered_map<std::string, double> symbol_sizes;
    std::unordered_map<std::string, int>    symbol_leverages;
    std::unordered_map<std::string, TfFilter> tf_filters;
};

class ExecutionEngine {
public:
    ExecutionEngine(SPSCQueue<WebhookSignal>& signal_queue,
                    BitgetAuth auth,
                    BitgetRestConfig rest_config,
                    RiskManager& risk_mgr,
                    SymbolScorer& scorer,
                    FeeAnalyzer& fee_analyzer,
                    PositionSizer& sizer,
                    PortfolioRiskManager& portfolio_risk,
                    StatePersistence& state_store,
                    AlertManager& alerts,
                    SymbolLearner& learner,
                    TradingConfig trading_config = {})
        : m_signal_queue(signal_queue)
        , m_auth(std::move(auth))
        , m_rest_config(std::move(rest_config))
        , m_risk(risk_mgr)
        , m_scorer(scorer)
        , m_fee(fee_analyzer)
        , m_sizer(sizer)
        , m_port_risk(portfolio_risk)
        , m_state(state_store)
        , m_alerts(alerts)
        , m_learner(learner)
        , m_trading(std::move(trading_config))
        , m_order_limiter(m_trading.order_rate_limit)
        , m_tpsl_limiter(m_trading.tpsl_rate_limit) {}

    void start() {
        // 상태 복구
        auto loaded = m_state.load_state();
        if (loaded.valid) {
            m_balance = loaded.balance;
            m_peak_balance = loaded.peak_balance;
            m_positions = std::move(loaded.positions);
            m_trades = std::move(loaded.trades);
            m_orders_executed.store(loaded.orders_executed, std::memory_order_relaxed);
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

        if (!m_trading.shadow_mode) {
            init_leverage();
        }

        int nw = std::max(1, m_trading.num_workers);
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
            m_state.save_state(m_balance, m_peak_balance, m_positions, m_trades, m_orders_executed.load());
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
        std::lock_guard lock(m_pos_mtx);
        return m_positions;
    }

    [[nodiscard]] nlohmann::json get_stats() const {
        std::lock_guard lock(m_pos_mtx);
        int total_trades = static_cast<int>(m_trades.size());
        int wins = 0; double total_pnl = 0;
        for (auto& t : m_trades) {
            if (t.pnl > 0) wins++;
            total_pnl += t.pnl;
        }
        return nlohmann::json{
            {"total_trades", total_trades},
            {"wins", wins},
            {"losses", total_trades - wins},
            {"win_rate", total_trades > 0 ? std::round(static_cast<double>(wins) / total_trades * 10000.0) / 100.0 : 0},
            {"total_pnl", std::round(total_pnl * 10000.0) / 10000.0},
            {"balance", m_balance},
            {"peak_balance", m_peak_balance},
            {"open_positions", m_positions.size()},
            {"orders_executed", m_orders_executed.load()},
            {"risk_skips", m_risk_skips.load()},
            {"shadow_mode", m_trading.shadow_mode}
        };
    }

    // Scorer access for dashboard
    SymbolScorer& scorer() { return m_scorer; }
    PortfolioRiskManager& portfolio_risk() { return m_port_risk; }
    AlertManager& alerts() { return m_alerts; }
    SymbolLearner& learner() { return m_learner; }

    [[nodiscard]] std::vector<TradeRecord> trades_snapshot() const {
        std::lock_guard lock(m_pos_mtx);
        return m_trades;
    }

private:
    // ── PnL 계산 헬퍼 ──
    static double calc_pnl(const std::string& side, double entry, double exit,
                           double qty, int leverage) {
        if (entry <= 0 || qty <= 0) return 0.0;
        double direction = (side == "long") ? 1.0 : -1.0;
        return direction * (exit - entry) * qty;
    }

    // ── 수수료 추정: taker 0.06% × 진입+청산 왕복 ──
    static double calc_fee(double price, double qty) {
        constexpr double TAKER_FEE_PCT = 0.0006;  // 0.06%
        double notional = price * qty;
        return notional * TAKER_FEE_PCT * 2.0;  // round-trip
    }

    void init_leverage() {
        net::io_context ioc;
        BitgetRestClient rest(ioc, m_auth, m_rest_config);

        // 실제 Bitget 잔고 조회
        fetch_real_balance(rest);

        // 거래소 포지션과 동기화 (ghost position 제거)
        sync_positions_with_exchange(rest);

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

    void fetch_real_balance(BitgetRestClient& rest) {
        try {
            auto account = rest.get_account();
            auto code = account.value("code", "99999");
            if (code == "00000" && account.contains("data")) {
                auto& data = account["data"];
                // Bitget V2: available = 사용 가능 잔고, accountEquity = 총 자산
                double available = 0.0;
                double equity = 0.0;
                if (data.contains("available")) {
                    available = std::stod(data["available"].get<std::string>());
                }
                if (data.contains("accountEquity")) {
                    equity = std::stod(data["accountEquity"].get<std::string>());
                }

                // 사용 가능 잔고를 기준으로 설정
                double real_balance = (available > 0) ? available : equity;
                if (real_balance > 0) {
                    std::lock_guard lock(m_pos_mtx);
                    m_balance = real_balance;
                    // peak_balance도 실제 잔고 기준으로 리셋
                    // (state에서 잘못된 값 로드된 경우 교정)
                    double real_equity = (equity > 0) ? equity : real_balance;
                    if (m_peak_balance > real_equity * 2.0 || m_peak_balance < real_balance) {
                        m_peak_balance = real_equity;
                        spdlog::info("[Exec] Peak balance corrected to {:.2f}", m_peak_balance);
                    }
                    m_port_risk.update_balance(m_balance);
                    spdlog::info("[Exec] Real Bitget balance: available={:.2f} equity={:.2f} -> using {:.2f}",
                        available, equity, m_balance);
                } else {
                    spdlog::warn("[Exec] Bitget returned zero balance, keeping default: {:.2f}", m_balance);
                }
            } else {
                spdlog::error("[Exec] Failed to fetch balance: code={} msg={}",
                    code, account.value("msg", "unknown"));
            }
        } catch (const std::exception& e) {
            spdlog::error("[Exec] Balance fetch error: {}", e.what());
        }
    }

    // -- 거래소 포지션 양방향 동기화 --
    // 1) Ghost 제거: 내부에 있지만 거래소에 없는 포지션 삭제
    // 2) 역방향 등록: 거래소에 있지만 내부에 없는 포지션 가져오기
    void sync_positions_with_exchange(BitgetRestClient& rest) {
        std::lock_guard lock(m_pos_mtx);

        try {
            auto resp = rest.get_positions();
            auto code = resp.value("code", "99999");
            if (code != "00000") {
                spdlog::error("[Sync] Failed to fetch exchange positions: code={} msg={}",
                    code, resp.value("msg", "unknown"));
                return;
            }

            // Parse exchange positions with full data
            struct ExchangePos {
                std::string symbol;
                std::string hold_side;
                double total{0.0};
                double avg_price{0.0};
                int leverage{10};
            };
            std::unordered_map<std::string, ExchangePos> exchange_positions;  // key: "SYMBOL:side"

            auto parse_double = [](const nlohmann::json& j, const std::string& key) -> double {
                if (!j.contains(key)) return 0.0;
                auto& v = j[key];
                if (v.is_string()) {
                    try { return std::stod(v.get<std::string>()); } catch (...) { return 0.0; }
                } else if (v.is_number()) {
                    return v.get<double>();
                }
                return 0.0;
            };
            auto parse_int = [](const nlohmann::json& j, const std::string& key, int def = 10) -> int {
                if (!j.contains(key)) return def;
                auto& v = j[key];
                if (v.is_string()) {
                    try { return std::stoi(v.get<std::string>()); } catch (...) { return def; }
                } else if (v.is_number()) {
                    return v.get<int>();
                }
                return def;
            };

            if (resp.contains("data") && resp["data"].is_array()) {
                for (auto& pos : resp["data"]) {
                    ExchangePos ep;
                    ep.symbol    = pos.value("symbol", "");
                    ep.hold_side = pos.value("holdSide", "");
                    ep.total     = parse_double(pos, "total");
                    ep.avg_price = parse_double(pos, "averageOpenPrice");
                    ep.leverage  = parse_int(pos, "leverage", 10);

                    if (!ep.symbol.empty() && ep.total > 0) {
                        std::string key = ep.symbol + ":" + ep.hold_side;
                        exchange_positions[key] = ep;
                    }
                }
            }

            spdlog::info("[Sync] Exchange has {} open position(s), internal has {}",
                exchange_positions.size(), m_positions.size());

            // === 1) Forward: Remove ghost positions (internal but not on exchange) ===
            std::vector<std::string> to_remove;
            for (auto& [key, pos] : m_positions) {
                std::string lookup = pos.symbol + ":" + pos.side;
                if (exchange_positions.find(lookup) == exchange_positions.end()) {
                    to_remove.push_back(key);
                    spdlog::warn("[Sync] Ghost position removed: key={} symbol={} side={} entry={:.2f} qty={:.6f}",
                        key, pos.symbol, pos.side, pos.entry_price, pos.quantity);
                }
            }
            for (auto& key : to_remove) {
                m_positions.erase(key);
            }

            // === 2) Reverse: Import exchange positions not in internal ===
            // Build set of internal symbol:side pairs for lookup
            std::unordered_set<std::string> internal_lookup;
            for (auto& [key, pos] : m_positions) {
                internal_lookup.insert(pos.symbol + ":" + pos.side);
            }

            int imported = 0;
            for (auto& [exkey, ep] : exchange_positions) {
                if (internal_lookup.find(exkey) == internal_lookup.end()) {
                    // This exchange position is not tracked internally — import it
                    ManagedPosition mp;
                    mp.symbol      = ep.symbol;
                    mp.timeframe   = "ext";  // 외부 동기화 마커
                    mp.side        = ep.hold_side;
                    mp.entry_price = ep.avg_price;
                    mp.quantity    = ep.total;
                    mp.leverage    = ep.leverage;
                    mp.tier        = "C";     // 기본 티어
                    mp.opened_at   = std::chrono::system_clock::now();

                    // Use symbol:side as internal key (consistent with exchange key format)
                    std::string internal_key = ep.symbol + "_" + ep.hold_side + "_ext";
                    m_positions[internal_key] = mp;
                    imported++;

                    spdlog::info("[Sync] Imported exchange position: {} {} entry={:.4f} qty={:.6f} lev={}x",
                        ep.symbol, ep.hold_side, ep.avg_price, ep.total, ep.leverage);
                }
            }

            // Log summary
            bool changed = !to_remove.empty() || imported > 0;
            if (changed) {
                spdlog::info("[Sync] Reconciliation: removed={} imported={} total={}",
                    to_remove.size(), imported, m_positions.size());
                m_state.save_state(m_balance, m_peak_balance, m_positions, m_trades, m_orders_executed.load());
            } else {
                spdlog::info("[Sync] All {} position(s) in sync", m_positions.size());
            }

        } catch (const std::exception& e) {
            spdlog::error("[Sync] Position sync error: {}", e.what());
        }
    }

    void start_websocket() {
        m_ws_client = std::make_unique<BitgetWSClient>(
            m_auth,
            // account 콜백 — 실시간 잔고 업데이트
            [this](const WsAccountUpdate& upd) {
                if (upd.available > 0) {
                    std::lock_guard lock(m_pos_mtx);
                    m_balance = upd.available;
                    if (upd.equity > m_peak_balance) m_peak_balance = upd.equity;
                    m_port_risk.update_balance(m_balance);
                }
            },
            // position 콜백 — 포지션 청산 감지 → 내부 상태 동기화 + PnL
            [this](const WsPositionUpdate& upd) {
                if (upd.size <= 0) {
                    spdlog::info("[WS] Position closed: {} {}", upd.symbol, upd.hold_side);
                    std::lock_guard lock(m_pos_mtx);
                    for (auto it = m_positions.begin(); it != m_positions.end(); ++it) {
                        if (it->second.symbol == upd.symbol && it->second.side == upd.hold_side) {
                            auto& pos = it->second;
                            double exit_price = upd.avg_price > 0 ? upd.avg_price : pos.entry_price;
                            double fee = calc_fee(exit_price, pos.quantity);
                            double pnl = calc_pnl(pos.side, pos.entry_price, exit_price,
                                                   pos.quantity, pos.leverage) - fee;
                            // Use exchange-reported PnL if available
                            if (std::abs(upd.realized_pnl) > 0.0001) {
                                pnl = upd.realized_pnl - fee;
                            }
                            TradeRecord tr;
                            tr.symbol = upd.symbol;
                            tr.timeframe = pos.timeframe;
                            tr.exit_reason = "WS_CLOSE";
                            tr.entry_price = pos.entry_price;
                            tr.exit_price = exit_price;
                            tr.quantity = pos.quantity;
                            tr.pnl = pnl;
                            tr.fee = fee;
                            m_trades.push_back(tr);
                            m_learner.record_trade(tr);
                            m_balance += pnl;
                            if (m_balance > m_peak_balance) m_peak_balance = m_balance;
                            spdlog::info("[WS] Auto-removed {} {} PnL={:.4f}",
                                upd.symbol, upd.hold_side, pnl);
                            m_alerts.info("TRADE", "WS close " + upd.symbol +
                                " PnL=" + std::to_string(pnl).substr(0,8), upd.symbol);
                            m_positions.erase(it);
                            m_state.save_state(m_balance, m_peak_balance, m_positions, m_trades, m_orders_executed.load());
                            break;
                        }
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
            auto sig_opt = m_signal_queue.wait_pop(std::chrono::milliseconds(500));
            if (!sig_opt) {
                if (wid == 0) periodic_tasks();
                continue;
            }
            process_signal(wid, rest, std::move(*sig_opt));
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
            m_state.save_state(m_balance, m_peak_balance, m_positions, m_trades, m_orders_executed.load());
        }

        // Periodic position sync with exchange (every 120 seconds)
        auto now_sec = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        auto last = m_last_sync_ts.load(std::memory_order_relaxed);
        if (now_sec - last >= 120) {
            m_last_sync_ts.store(now_sec, std::memory_order_relaxed);
            try {
                net::io_context sync_ioc;
                BitgetRestClient sync_rest(sync_ioc, m_auth, m_rest_config);
                sync_rest.set_contracts(m_contracts);
                sync_positions_with_exchange(sync_rest);
                fetch_real_balance(sync_rest);
                spdlog::info("[Periodic] Position sync + balance refresh done");
            } catch (const std::exception& e) {
                spdlog::warn("[Periodic] Sync failed: {}", e.what());
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
        spdlog::info("[Worker-{}] {} {} {} @ {:.2f} (lat: {:.1f}ms)",
            wid, signal_type_str(sig.sig_type), sig.action, sig.symbol,
            sig.price, latency_ms);

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
                m_orders_skipped.fetch_add(1);
                return;
            }
            m_recent_fingerprints.emplace(fp, now);
        }

        // 화이트리스트
        if (!m_trading.allowed_symbols.empty() &&
            !m_trading.allowed_symbols.count(sig.symbol)) {
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
            m_orders_skipped.fetch_add(1);
            return;
        }
        SymbolLockGuard guard(m_sym_locks, sig.symbol);

        std::string tf = sig.timeframe.empty() ? "unknown" : sig.timeframe;

        // 1. 티어 체크
        std::string tier = m_scorer.get_tier(sig.symbol);
        if (tier == "X" || tier == "D") {
            spdlog::info("[W-{}] SKIP {}: {}-tier", wid, sig.symbol, tier);
            m_risk_skips.fetch_add(1);
            return;
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

        // 2. TF 필터
        auto tf_it = m_trading.tf_filters.find(tf);
        if (tf_it != m_trading.tf_filters.end()) {
            auto& filt = tf_it->second;
            if (filt.block_reverse && sig.sig_type == SignalType::ReEntry) {
                m_risk_skips.fetch_add(1);
                return;
            }
            if (!tier_meets_min(tier, filt.min_tier)) {
                spdlog::info("[W-{}] SKIP {}: tier {} < {} for TF {}",
                    wid, sig.symbol, tier, filt.min_tier, tf);
                m_risk_skips.fetch_add(1);
                return;
            }
        }

        // 3. 수수료 수익성
        if (sig.has_tp1() && sig.price > 0) {
            double custom_ratio = 0;
            if (tf_it != m_trading.tf_filters.end())
                custom_ratio = tf_it->second.min_tp_to_cost_ratio;
            auto tp_chk = m_fee.is_tp_profitable(sig.symbol, sig.price, sig.tp1, custom_ratio);
            if (!tp_chk.profitable) {
                spdlog::info("[W-{}] SKIP {}: TP1 ratio={:.1f} < {:.1f}",
                    wid, sig.symbol, tp_chk.ratio, tp_chk.min_ratio);
                m_risk_skips.fetch_add(1);
                return;
            }
        }

        // 4. 동적 사이징 (가용 마진 기반)
        double balance_now, peak_now, dd_pct, used_margin;
        {
            std::lock_guard lock(m_pos_mtx);
            balance_now = m_balance;
            peak_now = m_peak_balance;
            dd_pct = peak_now > 0 ? (peak_now - balance_now) / peak_now * 100.0 : 0;

            // 오픈 포지션들의 사용 마진 합산
            used_margin = 0.0;
            for (auto& [key, pos] : m_positions) {
                double notional = pos.entry_price * pos.quantity;
                int pos_lev = pos.leverage > 0 ? pos.leverage : m_trading.default_leverage;
                used_margin += notional / pos_lev;
            }
        }

        const SymbolScore* score = m_scorer.get_score(sig.symbol);
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
            auto dec = m_port_risk.check_entry(
                sig.symbol, tf, sig.price, sig.size, leverage, m_balance, m_positions);
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
            m_orders_rejected.fetch_add(1);
            return;
        }

        spdlog::info("[W-{}] ENTRY {} {} sz={:.6f} lev={}x ${:.2f} tier={} | {}",
            wid, sig.action, sig.symbol, sig.size, leverage, sz.usdt_amount, tier, sz.reason);

        auto register_position = [&](uint64_t id) {
            std::lock_guard lock(m_pos_mtx);
            std::string key = sig.symbol + "_" + tf + "_" + std::to_string(id);
            ManagedPosition pos;
            pos.symbol = sig.symbol;
            pos.timeframe = tf;
            pos.side = sig.action == "buy" ? "long" : "short";
            pos.entry_price = sig.price;
            pos.quantity = sig.size;
            pos.leverage = leverage;
            pos.sl_price = sl_price;
            pos.tp1_price = sig.has_tp1() ? sig.tp1 : 0;
            pos.tier = tier;
            pos.opened_at = std::chrono::system_clock::now();
            m_positions[key] = pos;
        };

        if (m_trading.shadow_mode) {
            spdlog::info("[W-{}] SHADOW {} {} sz={:.6f}", wid, sig.action, sig.symbol, sig.size);
            m_orders_executed.fetch_add(1);
            register_position(oid);
            return;
        }

        // 주문 실행 (preset TP/SL 사용하지 않음 — 별도 API로 확실하게 설정)
        m_order_limiter.acquire();
        m_risk.on_order_placed();
        auto resp = rest.place_futures_order(order_req);
        m_risk.on_order_done();

        if (resp.status == OrderStatus::New) {
            m_orders_executed.fetch_add(1);
            std::string side_str = sig.action == "buy" ? "long" : "short";
            m_risk.on_position_opened(sig.symbol, sig.size, side_str);
            register_position(oid);

            spdlog::info("[W-{}] OK {} {} sz={:.6f} exid={}",
                wid, sig.action, sig.symbol, sig.size, resp.exchange_order_id);
            m_alerts.info("TRADE", "ENTRY " + sig.action + " " + sig.symbol +
                " sz=" + std::to_string(sig.size).substr(0,8) + " lev=" + std::to_string(leverage) + "x",
                sig.symbol);

            // Fix 4: 주문 성공 즉시 상태 저장 (크래시 시 포지션 유실 방지)
            {
                std::lock_guard lock(m_pos_mtx);
                m_state.save_state(m_balance, m_peak_balance, m_positions, m_trades, m_orders_executed.load());
            }

            // Default TP/SL 주입: 학습된 값 또는 기본값 (1.5%/1%)
            if (!sig.has_tp1() && sig.price > 0) {
                double tp_pct = learn_dec.tp_pct;  // L2 학습값 또는 기본 1.5%
                if (sig.action == "buy")
                    sig.tp1 = sig.price * (1.0 + tp_pct);
                else
                    sig.tp1 = sig.price * (1.0 - tp_pct);
                spdlog::info("[W-{}] TP1 injected: {:.4f} ({}%)", wid, sig.tp1, tp_pct * 100.0);
            }
            if (!sig.has_sl() && sig.price > 0) {
                double sl_pct = learn_dec.sl_pct;  // L2 학습값 또는 기본 1%
                if (sig.action == "buy")
                    sig.sl = sig.price * (1.0 - sl_pct);
                else
                    sig.sl = sig.price * (1.0 + sl_pct);
                spdlog::info("[W-{}] SL injected: {:.4f} ({}%)", wid, sig.sl, sl_pct * 100.0);
            }

            // TP/SL 설정 (재시도 + 실패 시 긴급 청산)
            set_sfx_tpsl(wid, rest, sig);
        } else {
            m_orders_rejected.fetch_add(1);
            spdlog::error("[W-{}] FAIL: err={} {}", wid, resp.error_code,
                std::string(resp.error_msg.data()));
        }
    }

    // ── TP 시그널 처리: 부분/전체 청산 ──
    void handle_tp(int wid, BitgetRestClient& rest, const WebhookSignal& sig) {
        spdlog::info("[W-{}] TP {} {} @ {:.2f}", wid, sig.tp_level, sig.symbol, sig.price);

        // hold_side 결정: bull 시그널 = long 포지션 청산
        std::string hold_side = (sig.signal_direction == "bull") ? "long" : "short";

        // 매칭되는 포지션 찾기
        std::string matched_key;
        double pos_qty = 0;
        {
            std::lock_guard lock(m_pos_mtx);
            for (auto& [key, pos] : m_positions) {
                if (pos.symbol == sig.symbol && pos.side == hold_side) {
                    matched_key = key;
                    pos_qty = pos.quantity;
                    break;
                }
            }
        }

        if (matched_key.empty()) {
            spdlog::warn("[W-{}] TP {} no matching position for {} hold={}",
                wid, sig.tp_level, sig.symbol, hold_side);
            return;
        }

        // TP 레벨별 청산 비율: TP1=33%, TP2=33%, TP3=나머지 전부
        double close_ratio = 1.0;
        if (sig.tp_level == "TP1")      close_ratio = 0.33;
        else if (sig.tp_level == "TP2") close_ratio = 0.50;  // 남은 67%의 50% ≈ 33%

        double close_qty = pos_qty * close_ratio;

        m_order_limiter.acquire();
        auto resp = rest.close_partial(sig.symbol, close_qty, hold_side);

        if (resp.status == OrderStatus::New) {
            spdlog::info("[W-{}] TP {} closed {:.6f}/{:.6f} of {}",
                wid, sig.tp_level, close_qty, pos_qty, sig.symbol);

            std::lock_guard lock(m_pos_mtx);
            auto it = m_positions.find(matched_key);
            if (it != m_positions.end()) {
                auto& pos = it->second;
                double exit_price = sig.price > 0 ? sig.price : pos.entry_price;
                double fee = calc_fee(exit_price, close_qty);
                double pnl = calc_pnl(pos.side, pos.entry_price, exit_price, close_qty, pos.leverage) - fee;

                if (sig.tp_level == "TP3" || close_ratio >= 0.99) {
                    // 전체 청산 → 포지션 제거 + 거래 기록
                    TradeRecord tr;
                    tr.symbol = sig.symbol;
                    tr.timeframe = pos.timeframe;
                    tr.exit_reason = sig.tp_level;
                    tr.entry_price = pos.entry_price;
                    tr.exit_price = exit_price;
                    tr.quantity = close_qty;
                    tr.pnl = pnl;
                    tr.fee = fee;
                    m_trades.push_back(tr);
                    m_learner.record_trade(tr);
                    m_balance += pnl;
                    if (m_balance > m_peak_balance) m_peak_balance = m_balance;
                    m_positions.erase(it);
                    m_alerts.info("TRADE", sig.tp_level + " " + sig.symbol +
                        " PnL=" + std::to_string(pnl).substr(0,8), sig.symbol);
                } else {
                    // 부분 청산 → 수량 차감 + PnL 기록
                    TradeRecord tr;
                    tr.symbol = sig.symbol;
                    tr.timeframe = pos.timeframe;
                    tr.exit_reason = sig.tp_level + "_PARTIAL";
                    tr.entry_price = pos.entry_price;
                    tr.exit_price = exit_price;
                    tr.quantity = close_qty;
                    tr.pnl = pnl;
                    tr.fee = fee;
                    m_trades.push_back(tr);
                    m_learner.record_trade(tr);
                    m_balance += pnl;
                    if (m_balance > m_peak_balance) m_peak_balance = m_balance;
                    pos.quantity -= close_qty;
                    if (pos.quantity <= 0) {
                        m_positions.erase(it);
                    }
                }
                m_state.save_state(m_balance, m_peak_balance, m_positions, m_trades, m_orders_executed.load());
            }
        } else {
            spdlog::error("[W-{}] TP {} close failed: {} {}",
                wid, sig.tp_level, resp.error_code,
                std::string(resp.error_msg.data()));
        }
    }

    // ── SL 시그널 처리: 전체 청산 ──
    void handle_sl(int wid, BitgetRestClient& rest, const WebhookSignal& sig) {
        spdlog::info("[W-{}] SL {} @ {:.2f}", wid, sig.symbol, sig.price);

        std::string hold_side = (sig.signal_direction == "bull") ? "long" : "short";

        // 매칭 포지션 찾기
        std::string matched_key;
        {
            std::lock_guard lock(m_pos_mtx);
            for (auto& [key, pos] : m_positions) {
                if (pos.symbol == sig.symbol && pos.side == hold_side) {
                    matched_key = key;
                    break;
                }
            }
        }

        if (matched_key.empty()) {
            spdlog::warn("[W-{}] SL no matching position for {} hold={}",
                wid, sig.symbol, hold_side);
            return;
        }

        // flash close로 전체 청산
        m_order_limiter.acquire();
        bool closed = rest.flash_close_position(sig.symbol, hold_side);

        if (closed) {
            std::lock_guard lock(m_pos_mtx);
            auto it = m_positions.find(matched_key);
            if (it != m_positions.end()) {
                auto& pos = it->second;
                double exit_price = sig.price > 0 ? sig.price : pos.sl_price;
                double fee = calc_fee(exit_price, pos.quantity);
                double pnl = calc_pnl(pos.side, pos.entry_price, exit_price, pos.quantity, pos.leverage) - fee;
                TradeRecord tr;
                tr.symbol = sig.symbol;
                tr.timeframe = pos.timeframe;
                tr.exit_reason = "SL";
                tr.entry_price = pos.entry_price;
                tr.exit_price = exit_price;
                tr.quantity = pos.quantity;
                tr.pnl = pnl;
                tr.fee = fee;
                m_trades.push_back(tr);
                m_learner.record_trade(tr);
                m_balance += pnl;
                if (m_balance > m_peak_balance) m_peak_balance = m_balance;
                m_positions.erase(it);
                m_state.save_state(m_balance, m_peak_balance, m_positions, m_trades, m_orders_executed.load());
                m_alerts.warn("TRADE", "SL " + sig.symbol +
                    " PnL=" + std::to_string(pnl).substr(0,8), sig.symbol);
            }
            spdlog::info("[W-{}] SL closed {} hold={}", wid, sig.symbol, hold_side);
        } else {
            spdlog::error("[W-{}] SL close FAILED for {} hold={}", wid, sig.symbol, hold_side);
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
            m_order_limiter.acquire();
            rest.flash_close_position(sig.symbol, hold_side);

            // 내부 상태에서도 포지션 제거
            std::lock_guard lock(m_pos_mtx);
            for (auto it = m_positions.begin(); it != m_positions.end(); ++it) {
                if (it->second.symbol == sig.symbol && it->second.side == hold_side) {
                    auto& pos = it->second;
                    double exit_price = sig.price;  // 긴급청산은 현재가
                    double fee = calc_fee(exit_price, pos.quantity);
                    double pnl = calc_pnl(pos.side, pos.entry_price, exit_price, pos.quantity, pos.leverage) - fee;
                    TradeRecord tr;
                    tr.symbol = sig.symbol;
                    tr.timeframe = pos.timeframe;
                    tr.exit_reason = "EMERGENCY_CLOSE_NO_SL";
                    tr.entry_price = pos.entry_price;
                    tr.exit_price = exit_price;
                    tr.quantity = pos.quantity;
                    tr.pnl = pnl;
                    tr.fee = fee;
                    m_trades.push_back(tr);
                    m_learner.record_trade(tr);
                    m_balance += pnl;
                    if (m_balance > m_peak_balance) m_peak_balance = m_balance;
                    m_positions.erase(it);
                    m_alerts.critical("TRADE", "EMERGENCY CLOSE " + sig.symbol +
                        " PnL=" + std::to_string(pnl).substr(0,8), sig.symbol);
                    break;
                }
            }
            m_state.save_state(m_balance, m_peak_balance, m_positions, m_trades, m_orders_executed.load());
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
    SPSCQueue<WebhookSignal>& m_signal_queue;
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
    std::atomic<uint64_t> m_next_oid{1};

    std::atomic<uint64_t> m_orders_executed{0};
    std::atomic<uint64_t> m_orders_rejected{0};
    std::atomic<uint64_t> m_orders_skipped{0};
    std::atomic<uint64_t> m_risk_skips{0};

    std::mutex m_fp_mtx;
    std::unordered_map<size_t, int64_t> m_recent_fingerprints;  // fingerprint -> timestamp_ns

    std::atomic<int64_t> m_last_sync_ts{0};  // epoch seconds of last position sync

    mutable std::mutex m_pos_mtx;
    double m_balance{100.0};   // 안전 기본값; start() 시 Bitget 실잔고로 덮어씀
    double m_peak_balance{100.0};
    std::unordered_map<std::string, ManagedPosition> m_positions;
    std::vector<TradeRecord> m_trades;

    // WebSocket 실시간 클라이언트
    std::unique_ptr<BitgetWSClient> m_ws_client;
};

} // namespace hft

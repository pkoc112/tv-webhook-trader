// ============================================================================
// execution/execution_engine.hpp -- 멀티스레드 주문 실행 엔진 v4.0
// 2026-03-13 | 고급 리스크 엔진 통합
//
// v4.0 변경:
//   - SymbolScorer, FeeAnalyzer, PositionSizer, PortfolioRiskManager 통합
//   - ManagedPosition 기반 포지션 추적
//   - StatePersistence 연동 (상태 저장/복구)
//   - TF 필터 지원
//   - Shadow 모드 (로그만, 실주문 안함)
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
            m_port_risk.update_balance(m_balance);

            if (!m_trades.empty()) {
                m_scorer.rescore_all(m_trades);
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
            m_state.save_state(m_balance, m_peak_balance, m_positions, m_trades);
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

private:
    void init_leverage() {
        net::io_context ioc;
        BitgetRestClient rest(ioc, m_auth, m_rest_config);

        // 실제 Bitget 잔고 조회
        fetch_real_balance(rest);

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
            // position 콜백 — 로그만 (포지션 관리는 내부 추적)
            [this](const WsPositionUpdate& upd) {
                // 포지션 크기 0 = 청산됨
                if (upd.size <= 0) {
                    spdlog::info("[WS] Position closed: {} {}", upd.symbol, upd.hold_side);
                }
            },
            // order 콜백 — 체결 확인 및 PnL 추적
            [this](const WsOrderUpdate& upd) {
                if (upd.status == "full-fill" && upd.trade_side == "close") {
                    // 포지션 종료 체결 → PnL 기록
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
            m_state.save_state(m_balance, m_peak_balance, m_positions, m_trades);
        }
    }

    void process_signal(int wid, BitgetRestClient& rest, WebhookSignal&& sig) {
        auto latency_ns = now_ns() - sig.received_at;
        spdlog::info("[Worker-{}] {} {} {} @ {:.2f} (lat: {:.1f}ms)",
            wid, signal_type_str(sig.sig_type), sig.action, sig.symbol,
            sig.price, latency_ns / 1'000'000.0);

        // 중복 방지
        auto fp = sig.fingerprint();
        {
            std::lock_guard lock(m_fp_mtx);
            if (m_recent_fingerprints.count(fp)) {
                m_orders_skipped.fetch_add(1);
                return;
            }
            m_recent_fingerprints.insert(fp);
            if (m_recent_fingerprints.size() > 2000) m_recent_fingerprints.clear();
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
                handle_tp(wid, sig);
                break;
            case SignalType::SL:
                handle_sl(wid, sig);
                break;
            default:
                spdlog::warn("[Worker-{}] Unknown: {}", wid, sig.alert);
                break;
        }
    }

    // ── 진입: 고급 리스크 파이프라인 ──
    void handle_entry(int wid, BitgetRestClient& rest, WebhookSignal&& sig) {
        if (!m_sym_locks.wait_lock(sig.symbol, std::chrono::seconds(5))) {
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

        // 4. 동적 사이징
        double balance_now, peak_now, dd_pct;
        {
            std::lock_guard lock(m_pos_mtx);
            balance_now = m_balance;
            peak_now = m_peak_balance;
            dd_pct = peak_now > 0 ? (peak_now - balance_now) / peak_now * 100.0 : 0;
        }

        const SymbolScore* score = m_scorer.get_score(sig.symbol);
        int leverage = m_trading.default_leverage;
        if (score) leverage = std::min(leverage, score->max_leverage);

        double sl_price = sig.has_sl() ? sig.sl : 0;
        auto sz = m_sizer.calc_size(balance_now, sig.symbol, sig.price, sl_price, leverage, score, dd_pct);

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
            set_sfx_tpsl(wid, rest, sig);
        } else {
            m_orders_rejected.fetch_add(1);
            spdlog::error("[W-{}] FAIL: err={} {}", wid, resp.error_code,
                std::string(resp.error_msg.data()));
        }
    }

    void handle_tp(int wid, const WebhookSignal& sig) {
        spdlog::info("[W-{}] TP {} {} @ {:.2f}", wid, sig.tp_level, sig.symbol, sig.price);
    }

    void handle_sl(int wid, const WebhookSignal& sig) {
        spdlog::info("[W-{}] SL {} @ {:.2f}", wid, sig.symbol, sig.price);
    }

    void set_sfx_tpsl(int wid, BitgetRestClient& rest, const WebhookSignal& sig) {
        try {
            std::string hold_side = sig.get_hold_side();
            if (sig.has_tp1()) {
                m_tpsl_limiter.acquire();
                rest.place_tpsl(sig.symbol, "profit_plan", sig.tp1, sig.size, hold_side);
            }
            if (sig.has_sl()) {
                m_tpsl_limiter.acquire();
                rest.place_tpsl(sig.symbol, "loss_plan", sig.sl, sig.size, hold_side);
            }
        } catch (const std::exception& e) {
            spdlog::error("[W-{}] TPSL fail: {}", wid, e.what());
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
    std::unordered_set<size_t> m_recent_fingerprints;

    mutable std::mutex m_pos_mtx;
    double m_balance{100.0};   // 안전 기본값; start() 시 Bitget 실잔고로 덮어씀
    double m_peak_balance{100.0};
    std::unordered_map<std::string, ManagedPosition> m_positions;
    std::vector<TradeRecord> m_trades;

    // WebSocket 실시간 클라이언트
    std::unique_ptr<BitgetWSClient> m_ws_client;
};

} // namespace hft

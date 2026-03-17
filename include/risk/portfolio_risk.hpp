// ============================================================================
// risk/portfolio_risk.hpp -- 포트폴리오 레벨 리스크 관리
// v1.0 | 2026-03-13 | Python sfx-trader risk/portfolio_risk.py 포팅
//
// 서킷 브레이커, TF별 포지션 제한, 노출 배분, 상관관계, 마진 체크
// ============================================================================
#pragma once

#include <string>
#include <unordered_map>
#include <deque>
#include <mutex>
#include <cmath>
#include <chrono>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace hft {

// -- 포지션 정보 (포트폴리오 관리용) --
struct ManagedPosition {
    std::string symbol;
    std::string timeframe;
    std::string side;
    double entry_price{0.0};
    double quantity{0.0};
    int    leverage{10};
    double sl_price{0.0};
    double tp1_price{0.0};
    std::string tier;
    std::string strategy{"unknown"};
    std::chrono::system_clock::time_point opened_at;
    bool   is_real{false};   // true=거래소 실제 포지션, false=shadow/paper
    std::string exchange{"bitget"};     // "bitget" / "upbit"
    std::string market_type{"futures"}; // "futures" / "spot"
};

struct RiskDecision {
    bool allowed{true};
    std::string reason;
    std::string check_failed;
};

inline void to_json(nlohmann::json& j, const RiskDecision& d) {
    j = nlohmann::json{
        {"allowed", d.allowed}, {"reason", d.reason},
        {"check_failed", d.check_failed}
    };
}

struct PortfolioState {
    int    total_positions{0};
    double total_notional{0.0};
    double total_margin_used{0.0};
    double margin_used_pct{0.0};
    std::unordered_map<std::string, int>    positions_by_tf;
    std::unordered_map<std::string, double> exposure_by_tf;
    double correlated_risk{0.0};
    double correlated_risk_pct{0.0};
    double daily_pnl{0.0};
    double weekly_pnl{0.0};
    double current_drawdown_pct{0.0};
    bool   circuit_breaker_active{false};
};

inline void to_json(nlohmann::json& j, const PortfolioState& s) {
    j = nlohmann::json{
        {"total_positions", s.total_positions},
        {"total_notional", s.total_notional},
        {"total_margin_used", s.total_margin_used},
        {"margin_used_pct", s.margin_used_pct},
        {"positions_by_tf", s.positions_by_tf},
        {"exposure_by_tf", s.exposure_by_tf},
        {"correlated_risk", s.correlated_risk},
        {"correlated_risk_pct", s.correlated_risk_pct},
        {"daily_pnl", s.daily_pnl},
        {"weekly_pnl", s.weekly_pnl},
        {"current_drawdown_pct", s.current_drawdown_pct},
        {"circuit_breaker_active", s.circuit_breaker_active}
    };
}


class PortfolioRiskManager {
public:
    explicit PortfolioRiskManager(const nlohmann::json& config) {
        auto cfg = config.value("portfolio_risk", nlohmann::json::object());
        m_max_pos_total = cfg.value("max_positions_total", 50);
        m_max_concurrent = cfg.value("max_concurrent_positions", 15);
        m_max_same_direction = cfg.value("max_same_direction", 10);

        // Shadow 모드용 완화된 한도 (학습 데이터 빠른 수집)
        auto shadow = cfg.value("shadow_limits", nlohmann::json::object());
        m_shadow_max_pos_total    = shadow.value("max_positions_total", 500);
        m_shadow_max_concurrent   = shadow.value("max_concurrent_positions", 200);
        m_shadow_max_same_dir     = shadow.value("max_same_direction", 100);
        m_shadow_max_margin_pct   = shadow.value("max_margin_usage_pct", 500.0);

        // Live 모드 최소 티어 (학습 완료된 심볼만 실전 진입)
        m_live_min_tier = cfg.value("live_min_tier", "B");

        if (cfg.contains("max_positions_per_tf")) {
            for (auto& [k, v] : cfg["max_positions_per_tf"].items())
                m_max_pos_per_tf[k] = v.get<int>();
        }
        if (cfg.contains("tf_exposure_pct")) {
            for (auto& [k, v] : cfg["tf_exposure_pct"].items())
                m_tf_exposure_pct[k] = v.get<double>();
        }

        m_corr_factor      = cfg.value("correlation_factor", 0.7);
        m_max_corr_risk_pct = cfg.value("max_correlated_risk_pct", 50.0);

        m_max_margin_usage_pct = cfg.value("max_margin_usage_pct", 90.0);
        m_rapid_loss_window_min = cfg.value("rapid_loss_window_minutes", 10);
        m_rapid_loss_pct = cfg.value("rapid_loss_pct", 2.0);

        auto cb = cfg.value("circuit_breaker", nlohmann::json::object());
        m_cb_daily_loss_pct  = cb.value("daily_loss_pct", 5.0);
        m_cb_weekly_loss_pct = cb.value("weekly_loss_pct", 10.0);
        m_cb_cooldown_min    = cb.value("cooldown_minutes", 60);
    }

    // ── Public API ──

    void set_shadow_mode(bool shadow) {
        std::lock_guard lock(m_mtx);
        m_shadow_mode = shadow;
        if (shadow) {
            spdlog::info("[RISK] Shadow mode: limits relaxed (concurrent={}, total={}, same_dir={})",
                m_shadow_max_concurrent, m_shadow_max_pos_total, m_shadow_max_same_dir);
        } else {
            spdlog::info("[RISK] Live mode: strict limits (concurrent={}, total={}, same_dir={}, min_tier={})",
                m_max_concurrent, m_max_pos_total, m_max_same_direction, m_live_min_tier);
        }
    }

    [[nodiscard]] bool is_shadow_mode() const {
        std::lock_guard lock(m_mtx);
        return m_shadow_mode;
    }

    [[nodiscard]] std::string get_live_min_tier() const {
        std::lock_guard lock(m_mtx);
        return m_live_min_tier;
    }

    void update_balance(double balance) {
        std::lock_guard lock(m_mtx);
        m_initial_balance = balance;
    }

    void on_trade_closed(double pnl) {
        std::lock_guard lock(m_mtx);
        check_daily_reset_locked();
        m_daily_pnl += pnl;
        m_weekly_pnl += pnl;

        // Track recent losses for rate-of-loss detection
        if (pnl < 0) {
            m_recent_losses.emplace_back(std::chrono::steady_clock::now(), pnl);
            prune_recent_losses_locked();

            double total_recent_loss = 0.0;
            for (auto& [_, loss] : m_recent_losses) total_recent_loss += loss;
            double rapid_limit = m_initial_balance * (m_rapid_loss_pct / 100.0);
            if (total_recent_loss < -rapid_limit) {
                activate_circuit_breaker_locked("rapid_loss");
            }
        }

        double daily_limit = m_initial_balance * (m_cb_daily_loss_pct / 100.0);
        if (m_daily_pnl < -daily_limit) {
            activate_circuit_breaker_locked("daily_loss");
        }
        double weekly_limit = m_initial_balance * (m_cb_weekly_loss_pct / 100.0);
        if (m_weekly_pnl < -weekly_limit) {
            activate_circuit_breaker_locked("weekly_loss");
        }
    }

    [[nodiscard]] RiskDecision check_entry(
        const std::string& symbol, const std::string& timeframe,
        double price, double qty, int leverage,
        double balance,
        const std::unordered_map<std::string, ManagedPosition>& positions,
        double sl_price = 0, const std::string& side = "long")
    {
        std::lock_guard lock(m_mtx);
        m_checks_total++;
        check_daily_reset_locked();

        // 0a. Hard notional cap (non-configurable safety backstop)
        if (price * qty > HARD_MAX_NOTIONAL) {
            return block("hard_notional_cap",
                "Notional " + fmt2(price * qty) + " > hard max " + fmt2(HARD_MAX_NOTIONAL));
        }

        // 0b. Liquidation price check
        if (leverage >= 10 && sl_price == 0) {
            spdlog::warn("[RISK] High leverage ({}x) with no SL for {}", leverage, symbol);
        }
        if (sl_price > 0 && leverage > 0) {
            bool is_long = (side == "long" || side == "buy");
            if (is_long) {
                double liq_est = price * (1.0 - 0.9 / leverage);
                if (sl_price < liq_est) {
                    return block("sl_beyond_liquidation",
                        "SL " + fmt2(sl_price) + " < est liq " + fmt2(liq_est) + " (LONG " + std::to_string(leverage) + "x)");
                }
            } else {
                double liq_est = price * (1.0 + 0.9 / leverage);
                if (sl_price > liq_est) {
                    return block("sl_beyond_liquidation",
                        "SL " + fmt2(sl_price) + " > est liq " + fmt2(liq_est) + " (SHORT " + std::to_string(leverage) + "x)");
                }
            }
        }

        // 1. 서킷 브레이커
        if (m_cb_active) {
            if (!check_cb_cooldown_locked()) {
                return block("circuit_breaker",
                    "Circuit breaker active (daily PnL: " + fmt2(m_daily_pnl) + ")");
            }
        }

        // 2a. HARD concurrent position limit
        //     Shadow mode: 완화된 한도 (학습 데이터 수집)
        //     Live mode: 엄격한 한도 (자본 보호)
        int total_pos = static_cast<int>(positions.size());
        int eff_max_concurrent = m_shadow_mode ? m_shadow_max_concurrent : m_max_concurrent;
        if (total_pos >= eff_max_concurrent) {
            return block("max_concurrent_positions",
                (m_shadow_mode ? "SHADOW " : "HARD ") + std::string("LIMIT: ")
                + std::to_string(total_pos) + "/" + std::to_string(eff_max_concurrent)
                + " concurrent positions");
        }

        // 2b. Same-direction concentration limit
        {
            bool is_long_entry = (side == "long" || side == "buy");
            std::string dir = is_long_entry ? "long" : "short";
            int same_dir_count = 0;
            for (auto& [_, p] : positions) {
                bool p_is_long = (p.side == "long" || p.side == "buy");
                if ((is_long_entry && p_is_long) || (!is_long_entry && !p_is_long))
                    same_dir_count++;
            }
            int eff_max_same_dir = m_shadow_mode ? m_shadow_max_same_dir : m_max_same_direction;
            if (same_dir_count >= eff_max_same_dir) {
                return block("max_same_direction",
                    dir + " concentration: " + std::to_string(same_dir_count)
                    + "/" + std::to_string(eff_max_same_dir));
            }
        }

        // 2c. Total positions cap
        int eff_max_total = m_shadow_mode ? m_shadow_max_pos_total : m_max_pos_total;
        if (total_pos >= eff_max_total) {
            return block("max_positions",
                "Max positions: " + std::to_string(total_pos) + "/" + std::to_string(eff_max_total));
        }

        // 3. TF별 포지션 제한
        std::string tf = timeframe.empty() ? "unknown" : timeframe;
        int tf_count = 0;
        for (auto& [_, p] : positions) {
            if (p.timeframe == tf) tf_count++;
        }
        int tf_limit = 10;
        auto it = m_max_pos_per_tf.find(tf);
        if (it != m_max_pos_per_tf.end()) tf_limit = it->second;

        if (tf_count >= tf_limit) {
            return block("tf_position_limit",
                "TF " + tf + "m limit: " + std::to_string(tf_count) + "/" + std::to_string(tf_limit));
        }

        // 4. TF별 노출 배분 (마진 기반)
        double tf_margin = 0.0;
        for (auto& [_, p] : positions) {
            if (p.timeframe == tf)
                tf_margin += std::abs(p.entry_price * p.quantity) / p.leverage;
        }
        double new_margin = price * qty / leverage;
        double tf_exp_pct = 30.0;
        auto eit = m_tf_exposure_pct.find(tf);
        if (eit != m_tf_exposure_pct.end()) tf_exp_pct = eit->second;
        double tf_max = balance * (tf_exp_pct / 100.0);

        if (tf_margin + new_margin > tf_max) {
            return block("tf_exposure",
                "TF " + tf + "m margin: " + fmt0(tf_margin + new_margin) + "/" + fmt0(tf_max));
        }

        // 5. 상관관계 리스크 (마진 기반)
        // corr_risk와 corr_limit 모두 마진(entry * qty / leverage) 기준으로 비교
        double total_margin_risk = 0.0;
        for (auto& [_, p] : positions) {
            total_margin_risk += std::abs(p.entry_price * p.quantity) / p.leverage;
        }
        double new_margin_risk = price * qty / leverage;
        double corr_risk = (total_margin_risk + new_margin_risk) * m_corr_factor;
        double corr_limit = balance * (m_max_corr_risk_pct / 100.0);
        if (corr_risk > corr_limit) {
            return block("correlation_risk",
                "Correlated risk: " + fmt0(corr_risk) + "/" + fmt0(corr_limit));
        }

        // 6. 마진 체크 (shadow 모드에서는 가상이므로 완화)
        double margin_needed = price * qty / leverage;
        double total_margin = 0.0;
        for (auto& [_, p] : positions) {
            total_margin += std::abs(p.entry_price * p.quantity) / p.leverage;
        }
        double eff_margin_pct = m_shadow_mode ? m_shadow_max_margin_pct : m_max_margin_usage_pct;
        double margin_limit = balance * (eff_margin_pct / 100.0);
        if (total_margin + margin_needed > margin_limit) {
            return block("margin_limit",
                "Margin " + fmt0(eff_margin_pct) + "%: " + fmt2(total_margin + margin_needed) + "/" + fmt2(margin_limit));
        }

        m_checks_passed++;
        return {true, "", ""};
    }

    [[nodiscard]] PortfolioState get_state(
        double balance,
        const std::unordered_map<std::string, ManagedPosition>& positions,
        double peak_balance = 0) const
    {
        std::lock_guard lock(m_mtx);

        PortfolioState state;
        state.total_positions = static_cast<int>(positions.size());

        for (auto& [_, p] : positions) {
            double notional = std::abs(p.entry_price * p.quantity * p.leverage);
            double margin = std::abs(p.entry_price * p.quantity) / p.leverage;
            state.total_notional += notional;
            state.total_margin_used += margin;

            std::string tf = p.timeframe.empty() ? "unknown" : p.timeframe;
            state.positions_by_tf[tf]++;
            state.exposure_by_tf[tf] += notional;
        }

        state.margin_used_pct = balance > 0
            ? std::round(state.total_margin_used / balance * 1000.0) / 10.0 : 0;
        state.correlated_risk = state.total_margin_used * m_corr_factor;
        state.correlated_risk_pct = balance > 0
            ? std::round(state.correlated_risk / balance * 1000.0) / 10.0 : 0;

        double dd = 0.0;
        if (peak_balance > 0) dd = (peak_balance - balance) / peak_balance * 100.0;

        state.daily_pnl = std::round(m_daily_pnl * 10000.0) / 10000.0;
        state.weekly_pnl = std::round(m_weekly_pnl * 10000.0) / 10000.0;
        state.current_drawdown_pct = std::round(dd * 100.0) / 100.0;
        state.circuit_breaker_active = m_cb_active;

        // Round exposure values
        for (auto& [k, v] : state.exposure_by_tf) {
            v = std::round(v * 100.0) / 100.0;
        }
        state.total_notional = std::round(state.total_notional * 100.0) / 100.0;
        state.total_margin_used = std::round(state.total_margin_used * 100.0) / 100.0;

        return state;
    }

    [[nodiscard]] nlohmann::json get_check_stats() const {
        std::lock_guard lock(m_mtx);
        double pass_rate = m_checks_total > 0
            ? std::round(static_cast<double>(m_checks_passed) / m_checks_total * 1000.0) / 10.0 : 0;
        return nlohmann::json{
            {"total_checks", m_checks_total},
            {"passed", m_checks_passed},
            {"blocked", m_checks_blocked},
            {"pass_rate", pass_rate},
            {"block_reasons", m_block_reasons}
        };
    }

private:
    RiskDecision block(const std::string& check, const std::string& reason) {
        m_checks_blocked++;
        m_block_reasons[check] = m_block_reasons.value(check, 0) + 1;
        return {false, reason, check};
    }

    void activate_circuit_breaker_locked(const std::string& trigger) {
        if (!m_cb_active) {
            m_cb_active = true;
            m_cb_activated_at = std::chrono::steady_clock::now();
            spdlog::error("[RISK] CIRCUIT BREAKER! trigger={} daily={:.2f} weekly={:.2f}",
                trigger, m_daily_pnl, m_weekly_pnl);
        }
    }

    bool check_cb_cooldown_locked() {
        auto elapsed = std::chrono::duration_cast<std::chrono::minutes>(
            std::chrono::steady_clock::now() - m_cb_activated_at).count();
        if (elapsed >= m_cb_cooldown_min) {
            m_cb_active = false;
            spdlog::info("[RISK] Circuit breaker released ({}min cooldown)", m_cb_cooldown_min);
            return true;
        }
        return false;
    }

    void check_daily_reset_locked() {
        auto now = std::chrono::system_clock::now();
        auto now_t = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
#ifdef _WIN32
        gmtime_s(&tm, &now_t);
#else
        gmtime_r(&now_t, &tm);
#endif
        // 간단 일일 리셋: 날짜 변경 감지
        int today = tm.tm_yday;
        if (today != m_last_day) {
            m_daily_pnl = 0.0;
            m_last_day = today;
            // 월요일(wday==1)이면 주간 리셋
            if (tm.tm_wday == 1 && today != m_last_week_day) {
                m_weekly_pnl = 0.0;
                m_last_week_day = today;
            }
        }
    }

    void prune_recent_losses_locked() {
        auto cutoff = std::chrono::steady_clock::now()
            - std::chrono::minutes(m_rapid_loss_window_min);
        while (!m_recent_losses.empty() && m_recent_losses.front().first < cutoff) {
            m_recent_losses.pop_front();
        }
    }

    static std::string fmt0(double v) {
        char buf[32]; std::snprintf(buf, sizeof(buf), "%.0f", v); return buf;
    }
    static std::string fmt2(double v) {
        char buf[32]; std::snprintf(buf, sizeof(buf), "%.2f", v); return buf;
    }

    // Hard safety limit (non-configurable)
    static constexpr double HARD_MAX_NOTIONAL = 500.0;

    // State
    mutable std::mutex m_mtx;
    double m_daily_pnl{0.0};
    double m_weekly_pnl{0.0};
    double m_initial_balance{1000.0};
    bool   m_cb_active{false};
    std::chrono::steady_clock::time_point m_cb_activated_at;
    int    m_last_day{-1};
    int    m_last_week_day{-1};
    std::deque<std::pair<std::chrono::steady_clock::time_point, double>> m_recent_losses;

    // Stats
    int m_checks_total{0};
    int m_checks_passed{0};
    int m_checks_blocked{0};
    nlohmann::json m_block_reasons = nlohmann::json::object();

    // Config
    int    m_max_pos_total{50};
    int    m_max_concurrent{15};       // HARD limit on total open positions
    int    m_max_same_direction{10};   // Max positions in same direction (long/short)

    // Shadow mode (학습 데이터 수집용 완화 한도)
    bool   m_shadow_mode{false};
    int    m_shadow_max_pos_total{500};
    int    m_shadow_max_concurrent{200};
    int    m_shadow_max_same_dir{100};
    double m_shadow_max_margin_pct{500.0};
    std::string m_live_min_tier{"B"};  // Live 모드 최소 허용 티어
    std::unordered_map<std::string, int>    m_max_pos_per_tf;
    std::unordered_map<std::string, double> m_tf_exposure_pct;
    double m_corr_factor{0.7};
    double m_max_corr_risk_pct{50.0};
    double m_max_margin_usage_pct{90.0};
    int    m_rapid_loss_window_min{10};
    double m_rapid_loss_pct{2.0};
    double m_cb_daily_loss_pct{5.0};
    double m_cb_weekly_loss_pct{10.0};
    int    m_cb_cooldown_min{60};
};

} // namespace hft

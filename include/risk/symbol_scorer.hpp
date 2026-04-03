// ============================================================================
// risk/symbol_scorer.hpp -- 심볼 스코어링 & 티어링 엔진
// v1.0 | 2026-03-13 | Python sfx-trader analysis/symbol_scorer.py 포팅
//
// 심볼별 성과 분석 → 복합 점수 (0-100) → S/A/B/C/D/X 티어 배정
// 가중치: expectancy(35%) + profit_factor(25%) + win_rate(20%)
//       + consistency(10%) + frequency_quality(10%)
// ============================================================================
#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <optional>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <chrono>
#include <fstream>
#include <filesystem>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace hft {

// -- 거래 기록 (JSON 역직렬화용) --
struct TradeRecord {
    std::string symbol;
    std::string timeframe;
    std::string exit_reason;
    double pnl{0.0};
    double fee{0.0};
    double entry_price{0.0};
    double exit_price{0.0};
    double quantity{0.0};
    std::string strategy{"unknown"};
    std::string exchange{"bitget"};     // "bitget" / "upbit"
    std::string market_type{"futures"}; // "futures" / "spot"

    // ── Realized RR fields (Feature: Realized RR Scoring) ──
    double gross_pnl{0.0};       // PnL before fees
    double net_pnl{0.0};         // PnL after fees (= gross_pnl - fee_cost)
    double fee_cost{0.0};        // entry fee + exit fee
    double realized_rr{0.0};     // actual_pnl_net / planned_risk
};

inline void from_json(const nlohmann::json& j, TradeRecord& t) {
    try {
        t.symbol      = j.value("symbol", "");
        t.timeframe   = j.value("timeframe", "");
        t.exit_reason = j.value("exit_reason", "");
        t.pnl         = j.value("pnl", 0.0);
        t.fee         = j.value("fee", 0.0);
        t.entry_price = j.value("entry_price", 0.0);
        t.exit_price  = j.value("exit_price", 0.0);
        t.quantity    = j.value("quantity", 0.0);
        t.strategy    = j.value("strategy", std::string("unknown"));
        t.exchange    = j.value("exchange", std::string("bitget"));
        t.market_type = j.value("market_type", std::string("futures"));
        // Realized RR fields (backward-compatible: default 0)
        t.gross_pnl    = j.value("gross_pnl", 0.0);
        t.net_pnl      = j.value("net_pnl", 0.0);
        t.fee_cost     = j.value("fee_cost", 0.0);
        t.realized_rr  = j.value("realized_rr", 0.0);
    } catch (...) {}
}

// -- TF별 서브 점수 --
struct TfSubScore {
    int trades{0};
    int wins{0};
    double win_rate{0.0};
    double pnl{0.0};
};

inline void to_json(nlohmann::json& j, const TfSubScore& s) {
    j = nlohmann::json{
        {"trades", s.trades}, {"wins", s.wins},
        {"win_rate", std::round(s.win_rate * 1000.0) / 10.0},
        {"pnl", std::round(s.pnl * 10000.0) / 10000.0}
    };
}

inline void from_json(const nlohmann::json& j, TfSubScore& s) {
    try {
        s.trades  = j.value("trades", 0);
        s.wins    = j.value("wins", 0);
        s.win_rate = j.value("win_rate", 0.0) / 100.0; // stored as %, load as ratio
        s.pnl     = j.value("pnl", 0.0);
    } catch (...) {}
}

// -- 심볼 점수 --
struct SymbolScore {
    std::string symbol;
    std::string tier{"C"};
    int    total_trades{0};
    int    wins{0};
    int    losses{0};
    double win_rate{0.0};        // 0.0 - 1.0
    double total_pnl{0.0};
    double avg_pnl{0.0};
    double avg_win{0.0};
    double avg_loss{0.0};        // positive value
    double profit_factor{0.0};
    double expectancy{0.0};
    double consistency{0.0};
    int    signal_count{0};
    double frequency_quality{0.0};
    double composite_score{0.0}; // 0-100
    bool   data_sufficient{false};
    double size_multiplier{0.5};
    int    max_leverage{10};
    std::string last_updated;
    std::unordered_map<std::string, TfSubScore> tf_scores;

    // ── Realized RR metrics (Feature: Realized RR Scoring) ──
    double avg_realized_rr{0.0};           // average realized risk-reward ratio
    double net_expectancy_per_trade{0.0};  // average net_pnl per trade
    double total_fees{0.0};                // total fee cost across all trades
    double realized_rr_consistency{0.0};   // closeness of planned vs realized RR (0-1)
    int    realized_trade_count{0};        // trades with valid realized_rr data

    // REVERSE 성적
    int    rev_trades{0};
    int    rev_wins{0};
    double rev_pnl{0.0};
    double rev_win_rate{0.0};
    bool   rev_allowed{false};
    double rev_score_adj{0.0};
};

inline void to_json(nlohmann::json& j, const SymbolScore& s) {
    j = nlohmann::json{
        {"symbol", s.symbol}, {"tier", s.tier},
        {"total_trades", s.total_trades}, {"wins", s.wins}, {"losses", s.losses},
        {"win_rate", s.win_rate}, {"total_pnl", s.total_pnl},
        {"avg_pnl", s.avg_pnl}, {"avg_win", s.avg_win}, {"avg_loss", s.avg_loss},
        {"profit_factor", s.profit_factor}, {"expectancy", s.expectancy},
        {"consistency", s.consistency}, {"signal_count", s.signal_count},
        {"frequency_quality", s.frequency_quality},
        {"composite_score", s.composite_score},
        {"data_sufficient", s.data_sufficient},
        {"size_multiplier", s.size_multiplier}, {"max_leverage", s.max_leverage},
        {"last_updated", s.last_updated},
        {"tf_scores", s.tf_scores},
        {"avg_realized_rr", s.avg_realized_rr},
        {"net_expectancy_per_trade", s.net_expectancy_per_trade},
        {"total_fees", s.total_fees},
        {"realized_rr_consistency", s.realized_rr_consistency},
        {"realized_trade_count", s.realized_trade_count},
        {"rev_trades", s.rev_trades}, {"rev_wins", s.rev_wins},
        {"rev_pnl", s.rev_pnl}, {"rev_win_rate", s.rev_win_rate},
        {"rev_allowed", s.rev_allowed}, {"rev_score_adj", s.rev_score_adj}
    };
}

inline void from_json(const nlohmann::json& j, SymbolScore& s) {
    try {
        s.symbol            = j.value("symbol", "");
        s.tier              = j.value("tier", "C");
        s.total_trades      = j.value("total_trades", 0);
        s.wins              = j.value("wins", 0);
        s.losses            = j.value("losses", 0);
        s.win_rate          = j.value("win_rate", 0.0);
        s.total_pnl         = j.value("total_pnl", 0.0);
        s.avg_pnl           = j.value("avg_pnl", 0.0);
        s.avg_win           = j.value("avg_win", 0.0);
        s.avg_loss          = j.value("avg_loss", 0.0);
        s.profit_factor     = j.value("profit_factor", 0.0);
        s.expectancy        = j.value("expectancy", 0.0);
        s.consistency       = j.value("consistency", 0.0);
        s.signal_count      = j.value("signal_count", 0);
        s.frequency_quality = j.value("frequency_quality", 0.0);
        s.composite_score   = j.value("composite_score", 0.0);
        s.data_sufficient   = j.value("data_sufficient", false);
        s.size_multiplier   = j.value("size_multiplier", 0.5);
        s.max_leverage      = j.value("max_leverage", 10);
        s.last_updated      = j.value("last_updated", "");
        if (j.contains("tf_scores") && j["tf_scores"].is_object()) {
            s.tf_scores = j["tf_scores"].get<std::unordered_map<std::string, TfSubScore>>();
        }
        s.avg_realized_rr          = j.value("avg_realized_rr", 0.0);
        s.net_expectancy_per_trade = j.value("net_expectancy_per_trade", 0.0);
        s.total_fees               = j.value("total_fees", 0.0);
        s.realized_rr_consistency  = j.value("realized_rr_consistency", 0.0);
        s.realized_trade_count     = j.value("realized_trade_count", 0);
        s.rev_trades    = j.value("rev_trades", 0);
        s.rev_wins      = j.value("rev_wins", 0);
        s.rev_pnl       = j.value("rev_pnl", 0.0);
        s.rev_win_rate  = j.value("rev_win_rate", 0.0);
        s.rev_allowed   = j.value("rev_allowed", false);
        s.rev_score_adj = j.value("rev_score_adj", 0.0);
    } catch (const std::exception& e) {
        spdlog::warn("[SCORER] from_json error for {}: {}", s.symbol, e.what());
    }
}


class SymbolScorer {
public:
    explicit SymbolScorer(const nlohmann::json& config,
                          const std::string& data_dir = "data")
        : m_data_dir(data_dir)
    {
        auto cfg = config.value("symbol_scoring", nlohmann::json::object());
        m_enabled     = cfg.value("enabled", true);
        m_min_trades  = cfg.value("min_trades", 20);

        auto w = cfg.value("weights", nlohmann::json::object());
        m_w_expectancy = w.value("expectancy", 0.35);
        m_w_pf         = w.value("profit_factor", 0.25);
        m_w_wr         = w.value("win_rate", 0.20);
        m_w_con        = w.value("consistency", 0.10);
        m_w_fq         = w.value("frequency_quality", 0.10);

        auto th = cfg.value("tier_thresholds", nlohmann::json::object());
        m_th_S = th.value("S", 75.0);
        m_th_A = th.value("A", 60.0);
        m_th_B = th.value("B", 45.0);
        m_th_C = th.value("C", 30.0);
        m_th_D = th.value("D", 15.0);

        auto sm = cfg.value("tier_size_multiplier", nlohmann::json::object());
        for (auto& [k, v] : sm.items()) {
            if (!v.is_null() && v.is_number()) m_size_mult[k] = v.get<double>();
        }

        auto ml = cfg.value("tier_max_leverage", nlohmann::json::object());
        for (auto& [k, v] : ml.items()) {
            if (!v.is_null() && v.is_number()) m_max_lev[k] = v.get<int>();
        }

        // REVERSE 설정
        auto rev = config.value("reverse_filter", nlohmann::json::object());
        m_rev_enabled        = rev.value("enabled", true);
        m_rev_min_trades     = rev.value("min_trades", 3);
        m_rev_allow_wr       = rev.value("allow_win_rate", 0.35);
        m_rev_bonus_wr       = rev.value("bonus_win_rate", 0.50);
        m_rev_penalty_zero   = rev.value("penalty_zero_wins", -15.0);
        m_rev_penalty_low    = rev.value("penalty_low_wr", -10.0);
        m_rev_bonus          = rev.value("bonus_good_wr", 5.0);

        m_rescore_hours = cfg.value("rescore_interval_hours", 6);

        load_scores();
        load_context_scores();
    }

    // ── Public API ──

    // ========================================================================
    // Context Key System (Phase 1)
    // ========================================================================
    // Instead of one score per symbol, we also track scores per "context key":
    //   context_key = "SYMBOL:TIMEFRAME:DIRECTION"
    //   e.g., "BTCUSDT:15:long", "ETHUSDT:60:short"
    //
    // The same symbol can perform very differently across timeframes and
    // directions. Context scoring enables fine-grained risk management.
    //
    // Fallback: if no context data exists (or insufficient trades), the
    // system falls back to the existing symbol-level aggregate score.
    // ========================================================================

    /// Build a context key from components: "SYMBOL:TF:DIR"
    /// Direction should be "long" or "short". TF is the raw timeframe string.
    static std::string compute_context_key(const std::string& symbol,
                                           const std::string& timeframe,
                                           const std::string& direction) {
        std::string tf = timeframe.empty() ? "unknown" : timeframe;
        std::string dir = direction.empty() ? "unknown" : direction;
        return symbol + ":" + tf + ":" + dir;
    }

    /// Get score for a specific context (symbol x timeframe x direction).
    /// Returns nullopt if context data is missing or insufficient; caller
    /// should fall back to get_score(symbol).
    [[nodiscard]] std::optional<SymbolScore> get_context_score(
        const std::string& symbol,
        const std::string& timeframe,
        const std::string& direction) const
    {
        std::string key = compute_context_key(symbol, timeframe, direction);
        std::lock_guard lock(m_mtx);
        auto it = m_context_scores.find(key);
        if (it != m_context_scores.end()) return it->second;
        return std::nullopt;
    }

    /// Record a single completed trade into the per-context accumulator.
    /// Call this when a trade closes (TP/SL hit, manual close, etc.).
    /// The context score is recomputed incrementally from accumulated stats.
    void record_context_trade(const std::string& symbol,
                              const std::string& timeframe,
                              const std::string& direction,
                              double pnl, bool is_win)
    {
        std::string key = compute_context_key(symbol, timeframe, direction);
        std::lock_guard lock(m_mtx);

        auto& score = m_context_scores[key];
        score.symbol = key;  // context key as identifier
        score.total_trades++;
        if (is_win) score.wins++;
        score.losses = score.total_trades - score.wins;
        score.total_pnl += pnl;
        score.win_rate = score.total_trades > 0
            ? static_cast<double>(score.wins) / score.total_trades : 0.0;
        score.avg_pnl = score.total_trades > 0
            ? score.total_pnl / score.total_trades : 0.0;

        // Recompute tier and composite for context score
        score.data_sufficient = score.total_trades >= m_min_trades;
        if (score.total_trades > 0) {
            // Simplified composite: Bayesian shrinkage for small samples
            double raw_score = 30.0 + (score.win_rate - 0.33) * 50.0;
            raw_score = std::clamp(raw_score, 15.0, 55.0);
            double sample_conf = static_cast<double>(score.total_trades)
                / (static_cast<double>(score.total_trades) + 20.0);
            score.composite_score = round1(
                35.0 * (1.0 - sample_conf) + raw_score * sample_conf);
            score.composite_score = std::clamp(score.composite_score, 0.0, 100.0);
        } else {
            score.composite_score = 35.0;
        }
        score.tier = assign_tier(score.composite_score);
        score.size_multiplier = size_mult_for(score.tier);
        score.max_leverage = max_lev_for(score.tier);
        score.last_updated = current_iso_time();

        save_context_scores_locked();

        spdlog::debug("[SCORER] Context trade recorded: {} trades={} wr={:.1f}% tier={}",
            key, score.total_trades, score.win_rate * 100.0, score.tier);
    }

    /// Record a realized trade with fee/RR data into context scoring.
    /// Called when a shadow or live trade closes with realized RR info.
    void record_realized_trade(const std::string& symbol,
                               const std::string& timeframe,
                               const std::string& direction,
                               double gross_pnl, double fee_cost,
                               double realized_rr)
    {
        std::string key = compute_context_key(symbol, timeframe, direction);
        std::lock_guard lock(m_mtx);

        auto& acc = m_realized_accumulators[key];
        acc.total_gross_pnl += gross_pnl;
        acc.total_fee_cost += fee_cost;
        acc.total_net_pnl += (gross_pnl - fee_cost);
        acc.sum_realized_rr += realized_rr;
        acc.count++;

        // Update the context score with realized metrics
        auto ctx_it = m_context_scores.find(key);
        if (ctx_it != m_context_scores.end()) {
            auto& score = ctx_it->second;
            score.avg_realized_rr = acc.count > 0
                ? acc.sum_realized_rr / acc.count : 0.0;
            score.net_expectancy_per_trade = acc.count > 0
                ? acc.total_net_pnl / acc.count : 0.0;
            score.total_fees = acc.total_fee_cost;
            score.realized_trade_count = acc.count;

            // Realized RR consistency: compare planned expectancy vs net expectancy
            // 1.0 = perfectly consistent, 0.0 = very different
            if (acc.count >= 10 && std::abs(score.expectancy) > 0.0001) {
                double ratio = score.net_expectancy_per_trade / score.expectancy;
                score.realized_rr_consistency = std::clamp(
                    1.0 - std::abs(1.0 - ratio), 0.0, 1.0);
            }

            // GPT review: realized RR blending disabled for live scoring.
            // Metrics are tracked (shadow observation) but do NOT modify
            // composite_score/tier/size_multiplier. Too few data points at
            // current stage — blending would chase recent noise, not edge.
            // TODO: Re-enable when 100+ realized trades confirm stable pattern.
            // if (acc.count >= 10) { ... blending logic ... }

            save_context_scores_locked();
        }

        spdlog::debug("[SCORER] Realized trade recorded: {} count={} avg_rr={:.3f} net_exp={:.4f}",
            key, acc.count,
            acc.count > 0 ? acc.sum_realized_rr / acc.count : 0.0,
            acc.count > 0 ? acc.total_net_pnl / acc.count : 0.0);
    }

    /// Get the best available score: context-specific if sufficient data,
    /// otherwise fall back to symbol-level aggregate.
    [[nodiscard]] std::optional<SymbolScore> get_best_score(
        const std::string& symbol,
        const std::string& timeframe,
        const std::string& direction) const
    {
        // Try context score first (require minimum 5 trades for context to be meaningful)
        auto ctx = get_context_score(symbol, timeframe, direction);
        if (ctx && ctx->total_trades >= 5) return ctx;
        // Fall back to symbol-level
        return get_score(symbol);
    }

    /// Thread-safe snapshot of all context scores
    [[nodiscard]] std::unordered_map<std::string, SymbolScore> context_scores_snapshot() const {
        std::lock_guard lock(m_mtx);
        return m_context_scores;
    }

    /// Get all context scores as JSON for dashboard/API
    [[nodiscard]] nlohmann::json get_all_context_scores_json() const {
        std::lock_guard lock(m_mtx);
        std::vector<std::pair<std::string, SymbolScore>> sorted(
            m_context_scores.begin(), m_context_scores.end());
        std::sort(sorted.begin(), sorted.end(),
            [](auto& a, auto& b) { return a.second.composite_score > b.second.composite_score; });
        auto arr = nlohmann::json::array();
        for (auto& [_, s] : sorted) arr.push_back(s);
        return arr;
    }

    void rescore_all(const std::vector<TradeRecord>& trades) {
        if (!m_enabled) return;

        // 심볼별 그룹핑
        std::unordered_map<std::string, std::vector<const TradeRecord*>> by_symbol;
        for (auto& t : trades) {
            if (!t.symbol.empty()) by_symbol[t.symbol].push_back(&t);
        }

        std::lock_guard lock(m_mtx);
        m_scores.clear();
        for (auto& [sym, sym_trades] : by_symbol) {
            m_scores[sym] = score_symbol(sym, sym_trades, 0);
        }
        m_last_rescore = std::chrono::system_clock::now();
        save_scores_locked();

        // 통계 로그
        std::unordered_map<std::string, int> tier_counts;
        for (auto& [_, s] : m_scores) tier_counts[s.tier]++;
        std::string dist;
        for (auto& [t, c] : tier_counts)
            dist += t + ":" + std::to_string(c) + " ";
        spdlog::info("[SCORER] Rescored {} symbols | {}", m_scores.size(), dist);
    }

    [[nodiscard]] std::string get_tier(const std::string& symbol) const {
        std::lock_guard lock(m_mtx);
        auto it = m_scores.find(symbol);
        return it != m_scores.end() ? it->second.tier : "C";
    }

    [[nodiscard]] double get_size_multiplier(const std::string& symbol) const {
        std::lock_guard lock(m_mtx);
        auto it = m_scores.find(symbol);
        return it != m_scores.end() ? it->second.size_multiplier : 0.5;
    }

    [[nodiscard]] int get_max_leverage(const std::string& symbol) const {
        std::lock_guard lock(m_mtx);
        auto it = m_scores.find(symbol);
        return it != m_scores.end() ? it->second.max_leverage : 10;
    }

    [[nodiscard]] std::optional<SymbolScore> get_score(const std::string& symbol) const {
        std::lock_guard lock(m_mtx);
        auto it = m_scores.find(symbol);
        if (it != m_scores.end()) return it->second;
        return std::nullopt;
    }

    [[nodiscard]] bool needs_rescore() const {
        std::lock_guard lock(m_mtx);
        if (m_last_rescore == std::chrono::system_clock::time_point{}) return true;
        auto elapsed = std::chrono::system_clock::now() - m_last_rescore;
        return elapsed >= std::chrono::hours(m_rescore_hours);
    }

    [[nodiscard]] nlohmann::json get_all_scores_json() const {
        std::lock_guard lock(m_mtx);
        std::vector<std::pair<std::string, SymbolScore>> sorted_scores(
            m_scores.begin(), m_scores.end());
        std::sort(sorted_scores.begin(), sorted_scores.end(),
            [](auto& a, auto& b) { return a.second.composite_score > b.second.composite_score; });

        auto arr = nlohmann::json::array();
        for (auto& [_, s] : sorted_scores) arr.push_back(s);
        return arr;
    }

    // Thread-safe read of all scores
    std::unordered_map<std::string, SymbolScore> scores_snapshot() const {
        std::lock_guard lock(m_mtx);
        return m_scores;
    }

private:
    // ── Scoring Logic (mirrors Python _score_symbol) ──

    SymbolScore score_symbol(const std::string& symbol,
                             const std::vector<const TradeRecord*>& trades,
                             int signal_count)
    {
        int n = static_cast<int>(trades.size());
        std::vector<double> wins_list, losses_list, all_pnls;
        double total_pnl = 0.0;

        for (auto* t : trades) {
            all_pnls.push_back(t->pnl);
            total_pnl += t->pnl;
            if (t->pnl > 0) wins_list.push_back(t->pnl);
            else             losses_list.push_back(t->pnl);
        }

        int wins = static_cast<int>(wins_list.size());
        int losses = static_cast<int>(losses_list.size());
        double wr = n > 0 ? static_cast<double>(wins) / n : 0.0;
        double avg_pnl = n > 0 ? total_pnl / n : 0.0;
        double avg_win = !wins_list.empty()
            ? std::accumulate(wins_list.begin(), wins_list.end(), 0.0) / wins_list.size()
            : 0.0;
        double avg_loss = !losses_list.empty()
            ? std::abs(std::accumulate(losses_list.begin(), losses_list.end(), 0.0) / losses_list.size())
            : 0.0;

        double gross_profit = std::accumulate(wins_list.begin(), wins_list.end(), 0.0);
        double gross_loss = std::abs(std::accumulate(losses_list.begin(), losses_list.end(), 0.0));
        double pf = gross_loss > 0 ? gross_profit / gross_loss : 0.0;

        double expectancy = (wr * avg_win) - ((1.0 - wr) * avg_loss);

        // Consistency: 1 - (stdev / mean_abs)
        double consistency = 0.5;
        if (n >= 3 && !all_pnls.empty()) {
            double mean = total_pnl / n;
            double sq_sum = 0.0;
            for (double p : all_pnls) sq_sum += (p - mean) * (p - mean);
            double stdev = std::sqrt(sq_sum / (n - 1));
            double mean_abs = 0.0;
            for (double p : all_pnls) mean_abs += std::abs(p);
            mean_abs /= n;
            if (mean_abs > 0)
                consistency = std::clamp(1.0 - stdev / mean_abs, 0.0, 1.0);
        }

        // Frequency quality
        double fq = 0.5;
        if (signal_count > 0 && n > 0) {
            double tps = static_cast<double>(n) / signal_count;
            fq = wr * std::min(tps, 1.0);
        }

        bool data_sufficient = n >= m_min_trades;

        // Composite score (0-100)
        double composite;
        if (data_sufficient) {
            double exp_s = normalize(expectancy, -0.05, 0.15) * 100.0;
            double pf_s  = normalize(pf, 0.5, 3.0) * 100.0;
            double wr_s  = normalize(wr, 0.25, 0.65) * 100.0;
            double con_s = consistency * 100.0;
            double fq_s  = fq * 100.0;

            composite = exp_s * m_w_expectancy + pf_s * m_w_pf +
                        wr_s * m_w_wr + con_s * m_w_con + fq_s * m_w_fq;
            composite = std::clamp(composite, 0.0, 100.0);
        } else {
            // ★ FIX: Bayesian shrinkage for small samples
            // Pulls score toward prior (35.0) proportional to sample size confidence
            // sample_conf = n/(n+20) → approaches 1.0 as n grows
            if (n > 0) {
                double raw_score = 30.0 + (wr - 0.33) * 50.0;
                raw_score = std::clamp(raw_score, 15.0, 55.0);
                double sample_conf = static_cast<double>(n) / (static_cast<double>(n) + 20.0);
                composite = 35.0 * (1.0 - sample_conf) + raw_score * sample_conf;
                composite = std::clamp(composite, 15.0, 55.0);
            } else {
                composite = 35.0;
            }
        }

        // REVERSE 성적
        int rev_n = 0, rev_wins_n = 0;
        double rev_pnl = 0.0;
        for (auto* t : trades) {
            if (t->exit_reason.find("REVERSE") != std::string::npos) {
                rev_n++;
                if (t->pnl > 0) rev_wins_n++;
                rev_pnl += t->pnl;
            }
        }
        double rev_wr = rev_n > 0 ? static_cast<double>(rev_wins_n) / rev_n : 0.0;

        double rev_score_adj = 0.0;
        bool rev_allowed = false;
        if (rev_n >= m_rev_min_trades) {
            if (rev_wins_n == 0) {
                rev_score_adj = m_rev_penalty_zero;
            } else if (rev_wr < m_rev_allow_wr) {
                rev_score_adj = m_rev_penalty_low;
            } else if (rev_wr >= m_rev_bonus_wr) {
                rev_score_adj = m_rev_bonus;
                rev_allowed = true;
            } else {
                rev_allowed = true;
            }
        }

        composite = std::clamp(composite + rev_score_adj, 0.0, 100.0);

        // Tier assignment
        std::string tier = assign_tier(composite);

        // TF별 점수
        std::unordered_map<std::string, TfSubScore> tf_scores;
        std::unordered_map<std::string, std::vector<const TradeRecord*>> tf_groups;
        for (auto* t : trades) {
            std::string tf = t->timeframe.empty() ? "unknown" : t->timeframe;
            tf_groups[tf].push_back(t);
        }
        for (auto& [tf, tf_trades] : tf_groups) {
            TfSubScore tfs;
            tfs.trades = static_cast<int>(tf_trades.size());
            for (auto* t : tf_trades) {
                if (t->pnl > 0) tfs.wins++;
                tfs.pnl += t->pnl;
            }
            tfs.win_rate = tfs.trades > 0
                ? static_cast<double>(tfs.wins) / tfs.trades : 0.0;
            tf_scores[tf] = tfs;
        }

        // Build result
        SymbolScore score;
        score.symbol            = symbol;
        score.tier              = tier;
        score.total_trades      = n;
        score.wins              = wins;
        score.losses            = losses;
        score.win_rate          = round4(wr);
        score.total_pnl         = round4(total_pnl);
        score.avg_pnl           = round4(avg_pnl);
        score.avg_win           = round4(avg_win);
        score.avg_loss          = round4(avg_loss);
        score.profit_factor     = round3(pf);
        score.expectancy        = round6(expectancy);
        score.consistency       = round3(consistency);
        score.signal_count      = signal_count;
        score.frequency_quality = round3(fq);
        score.composite_score   = round1(composite);
        score.data_sufficient   = data_sufficient;
        score.size_multiplier   = size_mult_for(tier);
        score.max_leverage      = max_lev_for(tier);
        score.last_updated      = current_iso_time();
        score.tf_scores         = std::move(tf_scores);
        score.rev_trades        = rev_n;
        score.rev_wins          = rev_wins_n;
        score.rev_pnl           = round4(rev_pnl);
        score.rev_win_rate      = round4(rev_wr);
        score.rev_allowed       = rev_allowed;
        score.rev_score_adj     = round1(rev_score_adj);

        return score;
    }

    std::string assign_tier(double score) const {
        if (score >= m_th_S) return "S";
        if (score >= m_th_A) return "A";
        if (score >= m_th_B) return "B";
        if (score >= m_th_C) return "C";
        if (score >= m_th_D) return "D";
        return "X";
    }

    static double normalize(double value, double low, double high) {
        if (high <= low) return 0.5;
        return std::clamp((value - low) / (high - low), 0.0, 1.0);
    }

    double size_mult_for(const std::string& tier) const {
        auto it = m_size_mult.find(tier);
        return it != m_size_mult.end() ? it->second : 0.5;
    }

    int max_lev_for(const std::string& tier) const {
        auto it = m_max_lev.find(tier);
        return it != m_max_lev.end() ? it->second : 10;
    }

    // ── Persistence ──

    void save_scores_locked() {
        try {
            std::filesystem::create_directories(m_data_dir);
            nlohmann::json data;
            data["last_rescore"] = current_iso_time();
            data["total_symbols"] = m_scores.size();
            auto& scores_obj = data["scores"];
            for (auto& [sym, s] : m_scores) scores_obj[sym] = s;

            auto path = m_data_dir + "/symbol_scores.json";
            auto tmp  = path + ".tmp";
            {
                std::ofstream f(tmp);
                f << data.dump(2);
            }
            std::filesystem::rename(tmp, path);
        } catch (const std::exception& e) {
            spdlog::error("[SCORER] Save failed: {}", e.what());
        }
    }

    void load_scores() {
        auto path = m_data_dir + "/symbol_scores.json";
        if (!std::filesystem::exists(path)) return;
        try {
            std::ifstream f(path);
            auto data = nlohmann::json::parse(f);
            std::lock_guard lock(m_mtx);
            // IMPORTANT: store in variable to avoid dangling reference from temporary
            auto scores_json = data.value("scores", nlohmann::json::object());
            for (auto& [sym, sd] : scores_json.items()) {
                m_scores[sym] = sd.get<SymbolScore>();
            }
            spdlog::info("[SCORER] Loaded {} symbols", m_scores.size());
        } catch (const std::exception& e) {
            spdlog::error("[SCORER] Load failed: {}", e.what());
        }
    }

    // ── Context Score Persistence ──

    void save_context_scores_locked() {
        try {
            std::filesystem::create_directories(m_data_dir);
            nlohmann::json data;
            data["last_updated"] = current_iso_time();
            data["total_contexts"] = m_context_scores.size();
            auto& ctx_obj = data["context_scores"];
            for (auto& [key, s] : m_context_scores) ctx_obj[key] = s;

            auto path = m_data_dir + "/context_scores.json";
            auto tmp  = path + ".tmp";
            {
                std::ofstream f(tmp);
                f << data.dump(2);
            }
            std::filesystem::rename(tmp, path);
        } catch (const std::exception& e) {
            spdlog::error("[SCORER] Context scores save failed: {}", e.what());
        }
    }

    void load_context_scores() {
        auto path = m_data_dir + "/context_scores.json";
        if (!std::filesystem::exists(path)) return;
        try {
            std::ifstream f(path);
            auto data = nlohmann::json::parse(f);
            std::lock_guard lock(m_mtx);
            auto ctx_json = data.value("context_scores", nlohmann::json::object());
            for (auto& [key, sd] : ctx_json.items()) {
                m_context_scores[key] = sd.get<SymbolScore>();
            }
            spdlog::info("[SCORER] Loaded {} context scores", m_context_scores.size());
        } catch (const std::exception& e) {
            spdlog::error("[SCORER] Context scores load failed: {}", e.what());
        }
    }

    // ── Helpers ──

    static std::string current_iso_time() {
        auto now = std::chrono::system_clock::now();
        auto t = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
#ifdef _WIN32
        gmtime_s(&tm, &t);
#else
        gmtime_r(&t, &tm);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
        return buf;
    }

    static double round1(double v) { return std::round(v * 10.0) / 10.0; }
    static double round3(double v) { return std::round(v * 1000.0) / 1000.0; }
    static double round4(double v) { return std::round(v * 10000.0) / 10000.0; }
    static double round6(double v) { return std::round(v * 1000000.0) / 1000000.0; }

    // ── Realized RR accumulator (per context key) ──
    struct RealizedAccumulator {
        double total_gross_pnl{0.0};
        double total_fee_cost{0.0};
        double total_net_pnl{0.0};
        double sum_realized_rr{0.0};
        int count{0};
    };

    // ── State ──
    mutable std::mutex m_mtx;
    std::unordered_map<std::string, SymbolScore> m_scores;
    // Context scores: keyed by "SYMBOL:TF:DIR" (e.g., "BTCUSDT:15:long")
    std::unordered_map<std::string, SymbolScore> m_context_scores;
    // Realized RR accumulators: keyed by same context key
    std::unordered_map<std::string, RealizedAccumulator> m_realized_accumulators;
    std::chrono::system_clock::time_point m_last_rescore{};
    std::string m_data_dir;

    // Config
    bool m_enabled{true};
    int  m_min_trades{20};
    int  m_rescore_hours{6};
    double m_w_expectancy{0.35}, m_w_pf{0.25}, m_w_wr{0.20};
    double m_w_con{0.10}, m_w_fq{0.10};
    double m_th_S{75}, m_th_A{60}, m_th_B{45}, m_th_C{30}, m_th_D{15};
    std::unordered_map<std::string, double> m_size_mult;
    std::unordered_map<std::string, int>    m_max_lev;

    // REVERSE filter
    bool   m_rev_enabled{true};
    int    m_rev_min_trades{3};
    double m_rev_allow_wr{0.35};
    double m_rev_bonus_wr{0.50};
    double m_rev_penalty_zero{-15.0};
    double m_rev_penalty_low{-10.0};
    double m_rev_bonus{5.0};
};

} // namespace hft

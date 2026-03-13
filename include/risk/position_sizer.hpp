// ============================================================================
// risk/position_sizer.hpp -- 동적 포지션 사이징 엔진
// v1.0 | 2026-03-13 | Python sfx-trader risk/position_sizer.py 포팅
//
// Quarter-Kelly + Risk-per-trade 하이브리드
// 드로다운 단계별 축소 + 티어 배수 적용
// ============================================================================
#pragma once

#include <string>
#include <vector>
#include <cmath>
#include <algorithm>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "risk/symbol_scorer.hpp"

namespace hft {

struct SizeResult {
    double qty{0.0};
    double usdt_amount{0.0};
    int    leverage{0};
    std::string method_used;
    double kelly_fraction{0.0};
    double kelly_usdt{0.0};
    double risk_usdt{0.0};
    double tier_multiplier{0.0};
    double drawdown_multiplier{0.0};
    double final_multiplier{0.0};
    std::string reason;
};

inline void to_json(nlohmann::json& j, const SizeResult& s) {
    j = nlohmann::json{
        {"qty", s.qty}, {"usdt_amount", s.usdt_amount},
        {"leverage", s.leverage}, {"method_used", s.method_used},
        {"kelly_fraction", s.kelly_fraction}, {"kelly_usdt", s.kelly_usdt},
        {"risk_usdt", s.risk_usdt},
        {"tier_multiplier", s.tier_multiplier},
        {"drawdown_multiplier", s.drawdown_multiplier},
        {"final_multiplier", s.final_multiplier},
        {"reason", s.reason}
    };
}


class PositionSizer {
public:
    explicit PositionSizer(const nlohmann::json& config) {
        auto cfg = config.value("position_sizing", nlohmann::json::object());
        m_kelly_frac    = cfg.value("kelly_fraction", 0.25);
        m_max_risk_pct  = cfg.value("max_risk_per_trade_pct", 2.0) / 100.0;
        m_min_usdt      = cfg.value("min_trade_usdt", 5.0);
        m_max_usdt      = cfg.value("max_trade_usdt", 50.0);
        m_default_usdt  = cfg.value("default_trade_usdt", 10.0);

        if (cfg.contains("drawdown_levels")) {
            for (auto& lv : cfg["drawdown_levels"]) {
                m_dd_levels.push_back({
                    lv.value("threshold", 100.0),
                    lv.value("multiplier", 0.0)
                });
            }
        }
        if (m_dd_levels.empty()) {
            m_dd_levels = {{3.0, 1.0}, {5.0, 0.75}, {8.0, 0.50}, {12.0, 0.25}, {100.0, 0.0}};
        }
    }

    // ── Public API ──

    [[nodiscard]] SizeResult calc_size(
        double balance, const std::string& symbol, double price,
        double sl_price, int leverage,
        const SymbolScore* score, double current_dd_pct) const
    {
        // 1. 심볼 스코어 파라미터
        double tier_mult = 0.5;
        int max_lev = 10;
        double win_rate = 0.0, avg_win = 0.0, avg_loss = 0.0;
        bool data_ok = false;
        std::string tier_str = "C";

        if (score) {
            tier_mult = score->size_multiplier;
            max_lev   = score->max_leverage;
            win_rate  = score->win_rate;
            avg_win   = score->avg_win;
            avg_loss  = score->avg_loss;
            data_ok   = score->data_sufficient;
            tier_str  = score->tier;
        }

        leverage = std::min(leverage, max_lev);

        // X티어 블랙리스트
        if (score && score->tier == "X") {
            return {0, 0, 0, "blocked", 0, 0, 0, 0, 0, 0,
                "X-tier blacklisted (score: " + std::to_string(score->composite_score) + ")"};
        }

        // 2. 드로다운 배수
        double dd_mult = drawdown_multiplier(current_dd_pct);
        if (dd_mult <= 0) {
            return {0, 0, leverage, "dd_stop", 0, 0, 0,
                tier_mult, 0, 0,
                "Drawdown " + std::to_string(current_dd_pct) + "% -> trading halted"};
        }

        // 3. 켈리 사이징
        double kelly_f = 0.0;
        double kelly_usdt = m_default_usdt;
        if (data_ok && win_rate > 0 && avg_loss > 0) {
            kelly_f = kelly_fraction(win_rate, avg_win, avg_loss);
            kelly_usdt = balance * kelly_f;
            kelly_usdt = std::clamp(kelly_usdt, m_min_usdt, m_max_usdt);
        }

        // 4. 리스크 기반 사이징
        double risk_usdt = m_default_usdt;
        std::string method = "default";
        if (sl_price > 0 && price > 0) {
            double sl_dist = std::abs(price - sl_price) / price;
            if (sl_dist > 0) {
                double risk_amount = balance * m_max_risk_pct;
                risk_usdt = risk_amount / (sl_dist * leverage);
                risk_usdt = std::clamp(risk_usdt, m_min_usdt, m_max_usdt);
                method = "risk_per_trade";
            }
        }

        // 5. 최종 금액
        double base_usdt;
        if (data_ok && kelly_f > 0) {
            base_usdt = std::min(kelly_usdt, risk_usdt);
            method = "kelly_risk_hybrid";
        } else if (sl_price > 0) {
            base_usdt = risk_usdt;
            method = "risk_per_trade";
        } else {
            base_usdt = m_default_usdt;
            method = "default";
        }

        // 6. 배수 적용
        double final_mult = tier_mult * dd_mult;
        double final_usdt = base_usdt * final_mult;
        final_usdt = std::clamp(final_usdt, m_min_usdt, m_max_usdt);

        // 미검증 심볼 캡
        if (!data_ok) {
            final_usdt = std::min(final_usdt, m_min_usdt);
        }

        // 7. 수량 변환
        double qty = price > 0 ? final_usdt / price : 0;

        // 이유 문자열
        std::string reason;
        if (data_ok) reason += "Kelly=" + fmt_pct(kelly_f);
        if (sl_price > 0) {
            if (!reason.empty()) reason += " | ";
            double sl_dist_pct = std::abs(price - sl_price) / price * 100.0;
            reason += "SL=" + fmt2(sl_dist_pct) + "%";
        }
        reason += " | Tier=" + tier_str + "(" + fmt2(tier_mult) + "x)";
        if (current_dd_pct > 0) {
            reason += " | DD=" + fmt1(current_dd_pct) + "%(" + fmt2(dd_mult) + "x)";
        }
        reason += " -> " + fmt1(final_usdt) + "USDT";

        return {qty, std::round(final_usdt * 100.0) / 100.0,
                leverage, method,
                std::round(kelly_f * 1000000.0) / 1000000.0,
                std::round(kelly_usdt * 100.0) / 100.0,
                std::round(risk_usdt * 100.0) / 100.0,
                tier_mult, dd_mult,
                std::round(final_mult * 10000.0) / 10000.0,
                reason};
    }

private:
    double kelly_fraction(double wr, double avg_w, double avg_l) const {
        if (avg_l <= 0 || avg_w <= 0) return 0.0;
        double R = avg_w / avg_l;
        double f = wr - (1.0 - wr) / R;
        if (f <= 0) return 0.0;
        f *= m_kelly_frac;
        return std::min(f, m_max_risk_pct);
    }

    double drawdown_multiplier(double dd_pct) const {
        for (auto& [threshold, mult] : m_dd_levels) {
            if (dd_pct <= threshold) return mult;
        }
        return 0.0;
    }

    static std::string fmt1(double v) {
        char buf[32]; std::snprintf(buf, sizeof(buf), "%.1f", v); return buf;
    }
    static std::string fmt2(double v) {
        char buf[32]; std::snprintf(buf, sizeof(buf), "%.2f", v); return buf;
    }
    static std::string fmt_pct(double v) {
        char buf[32]; std::snprintf(buf, sizeof(buf), "%.2f%%", v * 100.0); return buf;
    }

    double m_kelly_frac{0.25};
    double m_max_risk_pct{0.02};
    double m_min_usdt{5.0};
    double m_max_usdt{50.0};
    double m_default_usdt{10.0};

    struct DDLevel {
        double threshold;
        double multiplier;
    };
    std::vector<DDLevel> m_dd_levels;
};

} // namespace hft

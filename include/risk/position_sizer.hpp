// ============================================================================
// risk/position_sizer.hpp -- 동적 포지션 사이징 엔진
// v2.0 | 2026-03-15 | 잔고 비율 기반 동적 사이징
//
// 핵심 변경:
//   - 고정 USDT → 가용 잔고(available balance) 대비 % 계산
//   - available = balance - used_margin (오픈 포지션 마진 차감)
//   - base_pct(잔고의 30%) × 티어 배수 × TF 배수 × 드로다운 배수
//   - Kelly/Risk 데이터 있으면 하이브리드, 없으면 잔고 비율
// ============================================================================
#pragma once

#include <string>
#include <vector>
#include <optional>
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
        m_max_risk_pct  = cfg.value("max_risk_per_trade_pct", 5.0) / 100.0;
        m_min_usdt      = cfg.value("min_trade_usdt", 5.0);
        m_max_pct       = cfg.value("max_trade_pct", 50.0) / 100.0;    // 잔고의 최대 50%
        m_base_pct      = cfg.value("base_trade_pct", 30.0) / 100.0;   // 잔고의 기본 30%

        // 하위 호환: 고정 USDT 값도 로드 (fallback)
        m_max_usdt_hard = cfg.value("max_trade_usdt", 100.0);

        if (cfg.contains("drawdown_levels")) {
            for (auto& lv : cfg["drawdown_levels"]) {
                m_dd_levels.push_back({
                    lv.value("threshold", 100.0),
                    lv.value("multiplier", 0.0)
                });
            }
        }
        if (m_dd_levels.empty()) {
            m_dd_levels = {{10.0, 1.0}, {20.0, 0.75}, {30.0, 0.50}, {50.0, 0.25}, {100.0, 0.0}};
        }
    }

    // ── Public API ──

    // used_margin: 현재 오픈 포지션들이 사용 중인 마진 합계
    [[nodiscard]] SizeResult calc_size(
        double balance, const std::string& symbol, double price,
        double sl_price, int leverage,
        const std::optional<SymbolScore>& score, double current_dd_pct,
        double used_margin = 0.0) const
    {
        // 0. 가용 잔고 계산
        double available = std::max(balance - used_margin, 0.0);

        // 1. 심볼 스코어 파라미터
        double tier_mult = 1.0;
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

        // 3. 기본 사이즈 = 가용 잔고의 base_pct%
        double base_usdt = available * m_base_pct;
        std::string method = "balance_pct";

        // 4. 켈리 사이징 (데이터 충분할 때)
        double kelly_f = 0.0;
        double kelly_usdt = base_usdt;
        if (data_ok && win_rate > 0 && avg_loss > 0) {
            kelly_f = kelly_fraction(win_rate, avg_win, avg_loss);
            kelly_usdt = balance * kelly_f;  // 켈리는 전체 잔고 기준
            kelly_usdt = std::max(kelly_usdt, m_min_usdt);
        }

        // 5. 리스크 기반 사이징 (SL 있을 때)
        double risk_usdt = base_usdt;
        if (sl_price > 0 && price > 0) {
            double sl_dist = std::abs(price - sl_price) / price;
            if (sl_dist > 0) {
                double risk_amount = balance * m_max_risk_pct;
                risk_usdt = risk_amount / (sl_dist * leverage);
                risk_usdt = std::max(risk_usdt, m_min_usdt);
                method = "risk_per_trade";
            }
        }

        // 6. 최종 기본 금액 결정
        if (data_ok && kelly_f > 0) {
            base_usdt = std::min({kelly_usdt, risk_usdt, available * m_max_pct});
            method = "kelly_risk_hybrid";
        } else if (sl_price > 0 && price > 0) {
            base_usdt = std::min(risk_usdt, available * m_max_pct);
            method = "risk_per_trade";
        } else {
            // 기본: 가용 잔고의 base_pct%
            base_usdt = std::min(base_usdt, available * m_max_pct);
            method = "balance_pct";
        }

        // 7. 배수 적용 (티어 × 드로다운)
        double final_mult = tier_mult * dd_mult;
        double final_usdt = base_usdt * final_mult;

        // 최소/최대 클램프
        final_usdt = std::max(final_usdt, m_min_usdt);
        final_usdt = std::min(final_usdt, available * m_max_pct);  // 가용 잔고 초과 불가
        final_usdt = std::min(final_usdt, m_max_usdt_hard);        // 절대 한도

        // 잔고 부족 시
        if (available < m_min_usdt) {
            return {0, 0, leverage, "insufficient", 0, 0, 0,
                tier_mult, dd_mult, final_mult,
                "Insufficient available margin: " + fmt1(available) + " USDT"};
        }

        // 8. 수량 변환
        double qty = price > 0 ? final_usdt / price : 0;

        // 이유 문자열
        std::string reason = "Avail=" + fmt1(available) + "USDT";
        reason += "(" + fmt1(m_base_pct * 100) + "%->" + fmt1(base_usdt) + ")";
        if (data_ok) reason += " | Kelly=" + fmt_pct(kelly_f);
        if (sl_price > 0) {
            double sl_dist_pct = std::abs(price - sl_price) / price * 100.0;
            reason += " | SL=" + fmt2(sl_dist_pct) + "%";
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
    double m_max_risk_pct{0.05};
    double m_min_usdt{5.0};
    double m_max_pct{0.50};         // 가용 잔고의 최대 50%
    double m_base_pct{0.30};        // 가용 잔고의 기본 30%
    double m_max_usdt_hard{100.0};  // 절대 상한

    struct DDLevel {
        double threshold;
        double multiplier;
    };
    std::vector<DDLevel> m_dd_levels;
};

} // namespace hft

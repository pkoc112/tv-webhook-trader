// ============================================================================
// risk/fee_analyzer.hpp -- 수수료 & 슬리피지 분석 엔진
// v1.0 | 2026-03-13 | Python sfx-trader analysis/fee_analyzer.py 포팅
//
// Bitget 수수료: Taker 0.06%, Maker 0.02%
// 슬리피지: major 0.5bps, mid 1.5bps, altcoin 3.0bps
// 손익분기 = 2 * (fee + slippage)  (레버리지 무관)
// ============================================================================
#pragma once

#include <string>
#include <unordered_set>
#include <cmath>
#include <algorithm>

#include <nlohmann/json.hpp>

namespace hft {

struct CostAnalysis {
    std::string symbol;
    std::string slippage_tier;   // "major", "mid", "altcoin"
    double taker_fee_pct{0.0};
    double maker_fee_pct{0.0};
    double slippage_pct{0.0};
    double round_trip_cost_pct{0.0};
    double break_even_pct{0.0};
    double break_even_price{0.0};
    double effective_cost_pct{0.0};
    double funding_rate_8h_pct{0.0};
    double funding_per_day_pct{0.0};
};

inline void to_json(nlohmann::json& j, const CostAnalysis& c) {
    j = nlohmann::json{
        {"symbol", c.symbol}, {"slippage_tier", c.slippage_tier},
        {"taker_fee_pct", c.taker_fee_pct}, {"maker_fee_pct", c.maker_fee_pct},
        {"slippage_pct", c.slippage_pct},
        {"round_trip_cost_pct", c.round_trip_cost_pct},
        {"break_even_pct", c.break_even_pct},
        {"break_even_price", c.break_even_price},
        {"effective_cost_pct", c.effective_cost_pct},
        {"funding_rate_8h_pct", c.funding_rate_8h_pct},
        {"funding_per_day_pct", c.funding_per_day_pct}
    };
}


class FeeAnalyzer {
public:
    explicit FeeAnalyzer(const nlohmann::json& config) {
        auto cfg = config.value("fee_analysis", nlohmann::json::object());
        m_taker_fee     = cfg.value("taker_fee_pct", 0.06) / 100.0;
        m_maker_fee     = cfg.value("maker_fee_pct", 0.02) / 100.0;
        m_funding_default = cfg.value("funding_rate_default_pct", 0.01) / 100.0;
        m_min_tp_ratio  = cfg.value("min_tp_to_cost_ratio", 2.0);

        auto slip = cfg.value("slippage_tiers", nlohmann::json::object());
        auto major_cfg = slip.value("major", nlohmann::json::object());
        if (major_cfg.contains("symbols")) {
            for (auto& s : major_cfg["symbols"])
                m_major_symbols.insert(s.get<std::string>());
        } else {
            m_major_symbols = {"BTCUSDT", "ETHUSDT", "SOLUSDT", "BNBUSDT", "XRPUSDT"};
        }
        m_slip_major = major_cfg.value("base_bps", 0.5) / 10000.0;
        m_slip_mid   = slip.value("mid", nlohmann::json::object()).value("base_bps", 1.5) / 10000.0;
        m_slip_alt   = slip.value("altcoin", nlohmann::json::object()).value("base_bps", 3.0) / 10000.0;
    }

    // ── Public API ──

    [[nodiscard]] std::string get_slippage_tier(const std::string& symbol) const {
        if (m_major_symbols.count(symbol)) return "major";
        return "altcoin";
    }

    [[nodiscard]] double get_slippage(const std::string& symbol) const {
        auto tier = get_slippage_tier(symbol);
        if (tier == "major") return m_slip_major;
        if (tier == "mid")   return m_slip_mid;
        return m_slip_alt;
    }

    [[nodiscard]] double round_trip_cost_pct(const std::string& symbol) const {
        return 2.0 * (m_taker_fee + get_slippage(symbol)) * 100.0;
    }

    // 손익분기 가격 변동 (레버리지 무관: 왕복비용 = 손익분기)
    struct BreakEven {
        double pct{0.0};
        double price_move{0.0};
    };

    [[nodiscard]] BreakEven break_even_move(const std::string& symbol,
                                             double price) const {
        double rt_cost = 2.0 * (m_taker_fee + get_slippage(symbol));
        return {
            .pct = rt_cost * 100.0,
            .price_move = price * rt_cost
        };
    }

    // TP1까지 거리가 손익분기 × min_ratio 이상인지 확인
    struct TpCheck {
        bool profitable{false};
        double tp_distance_pct{0.0};
        double break_even_pct{0.0};
        double ratio{0.0};
        double min_ratio{0.0};
    };

    [[nodiscard]] TpCheck is_tp_profitable(const std::string& symbol,
                                            double entry_price,
                                            double tp_price,
                                            double custom_min_ratio = 0.0) const {
        double tp_dist = std::abs(tp_price - entry_price) / entry_price * 100.0;
        auto be = break_even_move(symbol, entry_price);
        double ratio = be.pct > 0 ? tp_dist / be.pct : 999.0;
        double min_r = custom_min_ratio > 0 ? custom_min_ratio : m_min_tp_ratio;

        return {
            .profitable = ratio >= min_r,
            .tp_distance_pct = tp_dist,
            .break_even_pct = be.pct,
            .ratio = ratio,
            .min_ratio = min_r
        };
    }

    [[nodiscard]] CostAnalysis analyze(const std::string& symbol,
                                        double price, int leverage = 10) const {
        auto tier = get_slippage_tier(symbol);
        double slip = get_slippage(symbol);
        double rt_cost = 2.0 * (m_taker_fee + slip) * 100.0;
        double be_price = price * (rt_cost / 100.0);
        double eff_cost = rt_cost * leverage;

        CostAnalysis ca;
        ca.symbol = symbol;
        ca.slippage_tier = tier;
        ca.taker_fee_pct = m_taker_fee * 100.0;
        ca.maker_fee_pct = m_maker_fee * 100.0;
        ca.slippage_pct = slip * 100.0;
        ca.round_trip_cost_pct = rt_cost;
        ca.break_even_pct = rt_cost;
        ca.break_even_price = be_price;
        ca.effective_cost_pct = eff_cost;
        ca.funding_rate_8h_pct = m_funding_default * 100.0;
        ca.funding_per_day_pct = m_funding_default * 100.0 * 3.0;
        return ca;
    }

private:
    double m_taker_fee{0.0006};
    double m_maker_fee{0.0002};
    double m_funding_default{0.0001};
    double m_min_tp_ratio{2.0};
    std::unordered_set<std::string> m_major_symbols;
    double m_slip_major{0.000005};
    double m_slip_mid{0.000015};
    double m_slip_alt{0.00003};
};

} // namespace hft

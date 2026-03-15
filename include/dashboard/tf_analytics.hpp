// ============================================================================
// dashboard/tf_analytics.hpp -- TF별 성과 분석 엔진
// 2026-03-15 | 타임프레임별 승률/PnL/리스크 비교 분석
//
// 거래 기록(TradeRecord)을 TF별로 집계하여 어떤 타임프레임이
// 가장 수익성 있는지 실시간으로 분석.
//
// API: /api/tf/stats → TF별 통계 JSON
// ============================================================================
#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <nlohmann/json.hpp>
#include "risk/symbol_scorer.hpp"  // TradeRecord

namespace hft {

struct TfStats {
    std::string timeframe;
    int total_trades{0};
    int wins{0};
    int losses{0};
    double total_pnl{0.0};
    double total_fee{0.0};
    double max_win{0.0};
    double max_loss{0.0};
    double avg_pnl{0.0};
    double win_rate{0.0};
    double profit_factor{0.0};
    double avg_win{0.0};
    double avg_loss{0.0};
    int max_consecutive_wins{0};
    int max_consecutive_losses{0};
    // 최근 성과 (EMA)
    double ema_pnl{0.0};

    [[nodiscard]] nlohmann::json to_json() const {
        return nlohmann::json{
            {"timeframe", timeframe},
            {"total_trades", total_trades},
            {"wins", wins},
            {"losses", losses},
            {"win_rate", std::round(win_rate * 1000.0) / 10.0},  // as %
            {"total_pnl", std::round(total_pnl * 10000.0) / 10000.0},
            {"total_fee", std::round(total_fee * 10000.0) / 10000.0},
            {"net_pnl", std::round((total_pnl - total_fee) * 10000.0) / 10000.0},
            {"avg_pnl", std::round(avg_pnl * 10000.0) / 10000.0},
            {"max_win", std::round(max_win * 10000.0) / 10000.0},
            {"max_loss", std::round(max_loss * 10000.0) / 10000.0},
            {"profit_factor", std::round(profit_factor * 100.0) / 100.0},
            {"avg_win", std::round(avg_win * 10000.0) / 10000.0},
            {"avg_loss", std::round(avg_loss * 10000.0) / 10000.0},
            {"max_consecutive_wins", max_consecutive_wins},
            {"max_consecutive_losses", max_consecutive_losses},
            {"ema_pnl", std::round(ema_pnl * 10000.0) / 10000.0}
        };
    }
};

class TfAnalytics {
public:
    // 전체 거래 기록에서 TF별 통계 계산
    [[nodiscard]] static nlohmann::json analyze(const std::vector<TradeRecord>& trades) {
        // TF별로 거래 분류
        std::unordered_map<std::string, std::vector<const TradeRecord*>> by_tf;
        for (auto& t : trades) {
            std::string tf = t.timeframe.empty() ? "unknown" : t.timeframe;
            by_tf[tf].push_back(&t);
        }

        // 전체 통계 + TF별 통계
        auto overall = calc_stats(trades, "all");

        nlohmann::json result;
        result["overall"] = overall.to_json();

        auto tf_arr = nlohmann::json::array();
        // 정렬: 거래수 내림차순
        std::vector<std::pair<std::string, std::vector<const TradeRecord*>*>> sorted;
        for (auto& [tf, vec] : by_tf) sorted.push_back({tf, &vec});
        std::sort(sorted.begin(), sorted.end(),
            [](auto& a, auto& b) { return a.second->size() > b.second->size(); });

        for (auto& [tf, vec_ptr] : sorted) {
            // const TradeRecord* → TradeRecord 벡터로 변환하여 calc_stats 호출
            std::vector<TradeRecord> tf_trades;
            tf_trades.reserve(vec_ptr->size());
            for (auto* t : *vec_ptr) tf_trades.push_back(*t);
            auto stats = calc_stats(tf_trades, tf);
            tf_arr.push_back(stats.to_json());
        }
        result["by_timeframe"] = tf_arr;

        // TF 순위 (승률 기준, 최소 5건)
        auto ranking = nlohmann::json::array();
        std::vector<TfStats> ranked;
        for (auto& [tf, vec_ptr] : sorted) {
            if (vec_ptr->size() >= 5) {
                std::vector<TradeRecord> tf_trades;
                for (auto* t : *vec_ptr) tf_trades.push_back(*t);
                ranked.push_back(calc_stats(tf_trades, tf));
            }
        }
        // 순위: profit_factor 기준
        std::sort(ranked.begin(), ranked.end(),
            [](auto& a, auto& b) { return a.profit_factor > b.profit_factor; });
        for (size_t i = 0; i < ranked.size(); i++) {
            auto j = ranked[i].to_json();
            j["rank"] = i + 1;
            // 등급 부여
            if (ranked[i].profit_factor >= 2.0 && ranked[i].win_rate >= 0.5) j["grade"] = "S";
            else if (ranked[i].profit_factor >= 1.5 && ranked[i].win_rate >= 0.45) j["grade"] = "A";
            else if (ranked[i].profit_factor >= 1.2 && ranked[i].win_rate >= 0.4) j["grade"] = "B";
            else if (ranked[i].profit_factor >= 1.0) j["grade"] = "C";
            else j["grade"] = "D";
            ranking.push_back(j);
        }
        result["ranking"] = ranking;

        // 시간대별 승률 (TF × hour)
        result["hourly"] = calc_hourly_stats(trades);

        return result;
    }

private:
    static TfStats calc_stats(const std::vector<TradeRecord>& trades, const std::string& tf) {
        TfStats s;
        s.timeframe = tf;
        s.total_trades = static_cast<int>(trades.size());

        if (trades.empty()) return s;

        double gross_wins = 0, gross_losses = 0;
        int cur_wins = 0, cur_losses = 0;
        constexpr double EMA_ALPHA = 0.15;

        for (auto& t : trades) {
            s.total_pnl += t.pnl;
            s.total_fee += t.fee;

            if (t.pnl > 0) {
                s.wins++;
                gross_wins += t.pnl;
                s.max_win = std::max(s.max_win, t.pnl);
                cur_wins++;
                cur_losses = 0;
                s.max_consecutive_wins = std::max(s.max_consecutive_wins, cur_wins);
            } else if (t.pnl < 0) {
                s.losses++;
                gross_losses += std::abs(t.pnl);
                s.max_loss = std::min(s.max_loss, t.pnl);  // most negative
                cur_losses++;
                cur_wins = 0;
                s.max_consecutive_losses = std::max(s.max_consecutive_losses, cur_losses);
            }

            // EMA
            s.ema_pnl = EMA_ALPHA * t.pnl + (1.0 - EMA_ALPHA) * s.ema_pnl;
        }

        s.win_rate = s.total_trades > 0 ? static_cast<double>(s.wins) / s.total_trades : 0;
        s.avg_pnl = s.total_trades > 0 ? s.total_pnl / s.total_trades : 0;
        s.avg_win = s.wins > 0 ? gross_wins / s.wins : 0;
        s.avg_loss = s.losses > 0 ? gross_losses / s.losses : 0;
        s.profit_factor = gross_losses > 0 ? gross_wins / gross_losses : (gross_wins > 0 ? 999.0 : 0.0);

        return s;
    }

    // 시간대별 승률 분석 (0~23시)
    static nlohmann::json calc_hourly_stats(const std::vector<TradeRecord>& trades) {
        // TradeRecord에 timestamp가 없으므로 간단히 TF별 집계만 반환
        // TODO: TradeRecord에 timestamp 추가 시 시간대별 분석 가능
        return nlohmann::json::object();
    }
};

} // namespace hft

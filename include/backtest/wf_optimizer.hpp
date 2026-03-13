// ============================================================================
// backtest/wf_optimizer.hpp -- Walk-Forward 최적화 엔진
// v1.0 | 2026-03-13
//
// 아키텍처:
//   전체 시그널 시퀀스를 IS(In-Sample) + OOS(Out-of-Sample) 윈도우로 분할
//   IS에서 파라미터 그리드 탐색 → 최적 파라미터 선택 → OOS에서 검증
//   윈도우를 롤링하며 반복 → OOS 결과를 합산하여 최종 성과 산출
//
//   |---IS-1---|--OOS-1--|
//        |---IS-2---|--OOS-2--|
//             |---IS-3---|--OOS-3--|
//
// 최적화 대상:
//   - trade_amount_usdt: 건당 투입 USDT
//   - leverage: 레버리지 배수
//
// 목적 함수: Sharpe Ratio (과최적화 방지 + 리스크 조정)
// ============================================================================
#pragma once

#include <vector>
#include <algorithm>
#include <cmath>
#include <spdlog/spdlog.h>

#include "backtest/backtester.hpp"
#include "backtest/log_parser.hpp"

namespace hft::bt {

// -- WF 최적화 설정 --
struct WFConfig {
    // IS/OOS 비율
    double is_ratio{0.7};                // In-Sample 비율 (70%)
    int    min_is_signals{50};            // IS 최소 시그널 수
    int    min_oos_signals{20};           // OOS 최소 시그널 수
    int    step_size{0};                  // 윈도우 이동 단위 (0 = OOS 크기)

    // 파라미터 그리드
    std::vector<double> amount_grid{5, 10, 15, 20, 30, 50, 75, 100};
    std::vector<int>    leverage_grid{3, 5, 7, 10, 15, 20, 25};

    // 기본 백테스트 파라미터
    double fee_rate{0.0006};
    double initial_balance{1000.0};

    // 목적 함수 선택
    enum class Objective { Sharpe, PnL, ProfitFactor, ReturnOverDD };
    Objective objective{Objective::Sharpe};
};

// -- 단일 윈도우 WF 결과 --
struct WFWindowResult {
    int    window_id{0};
    int    is_start{0};
    int    is_end{0};
    int    oos_start{0};
    int    oos_end{0};

    // IS 최적 파라미터
    double best_amount{0.0};
    int    best_leverage{0};
    double is_sharpe{0.0};
    double is_pnl{0.0};

    // OOS 검증 결과
    BtMetrics oos_metrics;
};

// -- WF 최적화 최종 결과 --
struct WFResult {
    std::vector<WFWindowResult> windows;

    // 합산 OOS 성과
    double total_oos_pnl{0.0};
    double total_oos_return_pct{0.0};
    double avg_oos_sharpe{0.0};
    double max_oos_drawdown{0.0};
    int    total_oos_trades{0};
    double oos_win_rate{0.0};

    // 추천 파라미터 (OOS에서 가장 자주 선택된)
    double recommended_amount{0.0};
    int    recommended_leverage{0};

    void print() const {
        spdlog::info("============================================");
        spdlog::info("  Walk-Forward Optimization Results");
        spdlog::info("============================================");
        spdlog::info("  Windows:       {}", windows.size());
        spdlog::info("  OOS Total PnL: {:.2f} USDT ({:.2f}%)",
            total_oos_pnl, total_oos_return_pct);
        spdlog::info("  OOS Trades:    {}", total_oos_trades);
        spdlog::info("  OOS Win Rate:  {:.1f}%", oos_win_rate * 100);
        spdlog::info("  OOS Avg Sharpe:{:.3f}", avg_oos_sharpe);
        spdlog::info("  OOS Max DD:    {:.2f}%", max_oos_drawdown);
        spdlog::info("--------------------------------------------");
        spdlog::info("  Recommended:   {:.0f} USDT / {}x leverage",
            recommended_amount, recommended_leverage);
        spdlog::info("============================================");

        for (auto& w : windows) {
            spdlog::info("  [Window-{}] IS[{}-{}] OOS[{}-{}] | "
                "Params: {:.0f}/{} | IS Sharpe: {:.3f} | OOS PnL: {:.2f} Sharpe: {:.3f}",
                w.window_id, w.is_start, w.is_end, w.oos_start, w.oos_end,
                w.best_amount, w.best_leverage,
                w.is_sharpe, w.oos_metrics.total_pnl, w.oos_metrics.sharpe_ratio);
        }
    }
};

class WFOptimizer {
public:
    explicit WFOptimizer(WFConfig config = {})
        : m_config(std::move(config)) {}

    // -- Walk-Forward 최적화 실행 --
    WFResult optimize(const std::vector<BtSignal>& signals) {
        WFResult result;
        int n = static_cast<int>(signals.size());

        // 윈도우 크기 계산
        int window_size = n;  // 첫 윈도우는 전체 사용
        int is_size = static_cast<int>(window_size * m_config.is_ratio);
        int oos_size = window_size - is_size;

        // 고정 크기 윈도우로 변경
        if (n < m_config.min_is_signals + m_config.min_oos_signals) {
            spdlog::warn("[WF] Not enough signals ({}) for walk-forward. "
                "Need at least {}.", n, m_config.min_is_signals + m_config.min_oos_signals);
            // 시그널 부족 시 단순 그리드 서치로 대체
            return single_pass_optimize(signals);
        }

        // 윈도우 크기 결정 (IS 최소 + OOS 최소 기준)
        int total_window = std::max(
            m_config.min_is_signals + m_config.min_oos_signals,
            static_cast<int>(n * 0.5));  // 최소 전체의 50%

        is_size = static_cast<int>(total_window * m_config.is_ratio);
        oos_size = total_window - is_size;
        is_size = std::max(is_size, m_config.min_is_signals);
        oos_size = std::max(oos_size, m_config.min_oos_signals);

        int step = (m_config.step_size > 0) ? m_config.step_size : oos_size;

        spdlog::info("[WF] Signals: {} | IS: {} | OOS: {} | Step: {} | Grid: {}x{}",
            n, is_size, oos_size, step,
            m_config.amount_grid.size(), m_config.leverage_grid.size());

        // 롤링 윈도우 루프
        int window_id = 0;
        for (int start = 0; start + is_size + oos_size <= n; start += step) {
            int is_start  = start;
            int is_end    = start + is_size;
            int oos_start = is_end;
            int oos_end   = std::min(is_end + oos_size, n);

            // IS/OOS 시그널 추출
            std::vector<BtSignal> is_signals(
                signals.begin() + is_start, signals.begin() + is_end);
            std::vector<BtSignal> oos_signals(
                signals.begin() + oos_start, signals.begin() + oos_end);

            // IS: 그리드 서치
            auto [best_amount, best_leverage, best_score, best_pnl] =
                grid_search(is_signals);

            // OOS: 최적 파라미터로 검증
            Backtester bt(make_params(best_amount, best_leverage));
            auto oos_metrics = bt.run(oos_signals);
            oos_metrics.param_amount = best_amount;
            oos_metrics.param_leverage = best_leverage;

            // 결과 저장
            WFWindowResult wr;
            wr.window_id    = window_id++;
            wr.is_start     = is_start;
            wr.is_end       = is_end;
            wr.oos_start    = oos_start;
            wr.oos_end      = oos_end;
            wr.best_amount  = best_amount;
            wr.best_leverage = best_leverage;
            wr.is_sharpe    = best_score;
            wr.is_pnl       = best_pnl;
            wr.oos_metrics  = oos_metrics;

            result.windows.push_back(std::move(wr));
        }

        // 최종 결과 합산
        compute_final_results(result);
        return result;
    }

private:
    // -- 그리드 서치: IS 구간에서 최적 파라미터 탐색 --
    struct GridResult {
        double amount;
        int    leverage;
        double score;
        double pnl;
    };

    GridResult grid_search(const std::vector<BtSignal>& signals) {
        GridResult best{m_config.amount_grid[0], m_config.leverage_grid[0], -1e9, 0.0};

        for (double amount : m_config.amount_grid) {
            for (int lev : m_config.leverage_grid) {
                Backtester bt(make_params(amount, lev));
                auto metrics = bt.run(signals);
                double score = evaluate(metrics);

                if (score > best.score) {
                    best.amount   = amount;
                    best.leverage = lev;
                    best.score    = score;
                    best.pnl      = metrics.total_pnl;
                }
            }
        }

        return best;
    }

    // -- 목적 함수 평가 --
    double evaluate(const BtMetrics& m) const {
        switch (m_config.objective) {
            case WFConfig::Objective::Sharpe:
                // DD 페널티: DD 10% 초과 시 감점
                return m.sharpe_ratio - std::max(0.0, m.max_drawdown_pct - 10.0) * 0.1;

            case WFConfig::Objective::PnL:
                return m.total_pnl;

            case WFConfig::Objective::ProfitFactor:
                return m.profit_factor;

            case WFConfig::Objective::ReturnOverDD:
                return (m.max_drawdown_pct > 0.1) ?
                    m.total_return_pct / m.max_drawdown_pct : m.total_return_pct;
        }
        return 0.0;
    }

    // -- 시그널 부족 시 단순 그리드 서치 --
    WFResult single_pass_optimize(const std::vector<BtSignal>& signals) {
        spdlog::info("[WF] Falling back to single-pass grid search");
        WFResult result;

        auto [best_amount, best_leverage, best_score, best_pnl] = grid_search(signals);

        // 결과를 단일 윈도우로 저장
        Backtester bt(make_params(best_amount, best_leverage));
        auto metrics = bt.run(signals);

        WFWindowResult wr;
        wr.window_id     = 0;
        wr.is_start      = 0;
        wr.is_end        = static_cast<int>(signals.size());
        wr.oos_start     = 0;
        wr.oos_end       = static_cast<int>(signals.size());
        wr.best_amount   = best_amount;
        wr.best_leverage = best_leverage;
        wr.is_sharpe     = best_score;
        wr.is_pnl        = best_pnl;
        wr.oos_metrics   = metrics;
        result.windows.push_back(std::move(wr));

        result.recommended_amount  = best_amount;
        result.recommended_leverage = best_leverage;
        result.total_oos_pnl       = metrics.total_pnl;
        result.total_oos_return_pct = metrics.total_return_pct;
        result.avg_oos_sharpe      = metrics.sharpe_ratio;
        result.max_oos_drawdown    = metrics.max_drawdown_pct;
        result.total_oos_trades    = metrics.total_trades;
        result.oos_win_rate        = metrics.win_rate;

        return result;
    }

    // -- 최종 결과 합산 --
    void compute_final_results(WFResult& result) {
        if (result.windows.empty()) return;

        double sum_pnl = 0.0, sum_sharpe = 0.0;
        int sum_trades = 0, sum_wins = 0, sum_total = 0;

        // 파라미터 빈도 추적
        std::unordered_map<std::string, int> param_freq;

        for (auto& w : result.windows) {
            sum_pnl    += w.oos_metrics.total_pnl;
            sum_sharpe += w.oos_metrics.sharpe_ratio;
            sum_trades += w.oos_metrics.total_trades;
            sum_wins   += w.oos_metrics.wins;
            sum_total  += w.oos_metrics.total_trades;
            result.max_oos_drawdown = std::max(
                result.max_oos_drawdown, w.oos_metrics.max_drawdown_pct);

            // 파라미터 빈도
            auto key = std::to_string(static_cast<int>(w.best_amount))
                     + "_" + std::to_string(w.best_leverage);
            param_freq[key]++;
        }

        result.total_oos_pnl     = sum_pnl;
        result.total_oos_return_pct = (m_config.initial_balance > 0) ?
            sum_pnl / m_config.initial_balance * 100.0 : 0.0;
        result.avg_oos_sharpe    = sum_sharpe / result.windows.size();
        result.total_oos_trades  = sum_trades;
        result.oos_win_rate      = (sum_total > 0) ?
            static_cast<double>(sum_wins) / sum_total : 0.0;

        // 가장 빈번한 파라미터 = 추천
        int max_freq = 0;
        for (auto& [key, freq] : param_freq) {
            if (freq > max_freq) {
                max_freq = freq;
                auto sep = key.find('_');
                result.recommended_amount  = std::stod(key.substr(0, sep));
                result.recommended_leverage = std::stoi(key.substr(sep + 1));
            }
        }
    }

    BtParams make_params(double amount, int leverage) const {
        BtParams p;
        p.trade_amount_usdt = amount;
        p.leverage          = leverage;
        p.fee_rate          = m_config.fee_rate;
        p.initial_balance   = m_config.initial_balance;
        return p;
    }

    WFConfig m_config;
};

} // namespace hft::bt

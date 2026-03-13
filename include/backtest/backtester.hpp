// ============================================================================
// backtest/backtester.hpp -- 시그널 기반 백테스트 엔진
// v1.0 | 2026-03-13
//
// sfx-trader 시그널 시퀀스를 재생하며 다양한 비중/레버리지 파라미터로
// 가상 거래를 시뮬레이션. Walk-Forward 최적화의 핵심 모듈.
//
// 시뮬레이션 모델:
//   - 시장가 진입 (시그널 price 사용)
//   - TP1: 33% 부분 청산, TP2: 33%, TP3: 나머지 전량
//   - SL: 전량 청산
//   - 수수료: 0.06% (Bitget futures taker)
//   - 마진 = size * price / leverage
// ============================================================================
#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <spdlog/spdlog.h>

#include "backtest/log_parser.hpp"

namespace hft::bt {

// -- 백테스트 파라미터 --
struct BtParams {
    double trade_amount_usdt{10.0};     // 건당 투입 USDT
    int    leverage{10};                 // 레버리지
    double fee_rate{0.0006};             // 수수료율 (0.06%)
    double tp1_ratio{0.33};              // TP1 부분 청산 비율
    double tp2_ratio{0.33};              // TP2 부분 청산 비율
    // TP3 = 나머지 전량 (1 - tp1 - tp2)
    double initial_balance{1000.0};      // 초기 잔고
};

// -- 시뮬레이션 포지션 --
struct BtPosition {
    std::string symbol;
    std::string side;           // "long" or "short"
    double      entry_price{0.0};
    double      quantity{0.0};
    double      initial_qty{0.0};
    int         leverage{10};
    double      tp1{0.0};
    double      tp2{0.0};
    double      tp3{0.0};
    double      sl{0.0};
};

// -- 단일 시뮬레이션 거래 결과 --
struct BtTradeResult {
    std::string symbol;
    std::string side;
    double      entry_price{0.0};
    double      exit_price{0.0};
    double      quantity{0.0};
    double      pnl{0.0};
    double      pnl_pct{0.0};
    std::string exit_reason;
};

// -- 백테스트 성과 지표 --
struct BtMetrics {
    double total_pnl{0.0};
    double total_return_pct{0.0};
    int    total_trades{0};
    int    wins{0};
    int    losses{0};
    double win_rate{0.0};
    double avg_pnl{0.0};
    double max_drawdown_pct{0.0};
    double sharpe_ratio{0.0};
    double profit_factor{0.0};
    double final_balance{0.0};
    double avg_win{0.0};
    double avg_loss{0.0};

    // 파라미터 기록 (WF 결과 추적용)
    double param_amount{0.0};
    int    param_leverage{0};

    void print() const {
        spdlog::info("=== Backtest Results ===");
        spdlog::info("  Total PnL:      {:.2f} USDT ({:.2f}%)", total_pnl, total_return_pct);
        spdlog::info("  Final Balance:  {:.2f}", final_balance);
        spdlog::info("  Trades:         {} (W:{} L:{})", total_trades, wins, losses);
        spdlog::info("  Win Rate:       {:.1f}%", win_rate * 100);
        spdlog::info("  Avg Win:        {:.4f} | Avg Loss: {:.4f}", avg_win, avg_loss);
        spdlog::info("  Profit Factor:  {:.2f}", profit_factor);
        spdlog::info("  Max Drawdown:   {:.2f}%", max_drawdown_pct);
        spdlog::info("  Sharpe Ratio:   {:.3f}", sharpe_ratio);
        spdlog::info("  Params:         {:.0f} USDT / {}x leverage", param_amount, param_leverage);
    }
};

class Backtester {
public:
    explicit Backtester(BtParams params = {})
        : m_params(std::move(params)) {}

    // -- 시그널 시퀀스로 백테스트 실행 --
    BtMetrics run(const std::vector<BtSignal>& signals) {
        reset();

        for (auto& sig : signals) {
            process_signal(sig);
        }

        // 미청산 포지션 현재가로 강제 청산
        close_all_remaining();

        return compute_metrics();
    }

    // -- 특정 파라미터로 실행 (WF용 편의 함수) --
    BtMetrics run_with_params(const std::vector<BtSignal>& signals,
                              double amount_usdt, int leverage) {
        m_params.trade_amount_usdt = amount_usdt;
        m_params.leverage = leverage;
        auto metrics = run(signals);
        metrics.param_amount = amount_usdt;
        metrics.param_leverage = leverage;
        return metrics;
    }

private:
    void reset() {
        m_balance = m_params.initial_balance;
        m_peak_balance = m_balance;
        m_max_dd = 0.0;
        m_positions.clear();
        m_results.clear();
        m_equity_curve.clear();
        m_equity_curve.push_back(m_balance);
    }

    void process_signal(const BtSignal& sig) {
        if (sig.is_entry()) {
            handle_entry(sig);
        } else if (sig.is_tp()) {
            handle_tp(sig);
        } else if (sig.is_sl()) {
            handle_sl(sig);
        } else if (sig.is_reentry()) {
            handle_reentry(sig);
        }
    }

    void handle_entry(const BtSignal& sig) {
        // 기존 포지션이 있으면 반대 방향 시그널은 청산 후 진입
        auto it = m_positions.find(sig.symbol);
        if (it != m_positions.end()) {
            auto& pos = it->second;
            std::string expected_side = (sig.alert == "buy") ? "long" : "short";
            if (pos.side != expected_side) {
                // 반대 방향 → 기존 포지션 전량 청산
                close_position(sig.symbol, sig.price, "REVERSE");
            } else {
                // 같은 방향 → 무시 (중복 진입 방지)
                return;
            }
        }

        // 마진 체크
        double size = m_params.trade_amount_usdt / sig.price;
        double margin = m_params.trade_amount_usdt / m_params.leverage;
        if (margin > m_balance * 0.95) {
            return; // 잔고 부족
        }

        BtPosition pos;
        pos.symbol      = sig.symbol;
        pos.side        = (sig.alert == "buy") ? "long" : "short";
        pos.entry_price = sig.price;
        pos.quantity    = size;
        pos.initial_qty = size;
        pos.leverage    = m_params.leverage;

        // 진입 수수료
        double entry_fee = size * sig.price * m_params.fee_rate;
        m_balance -= entry_fee;

        m_positions[sig.symbol] = std::move(pos);
    }

    void handle_tp(const BtSignal& sig) {
        auto it = m_positions.find(sig.symbol);
        if (it == m_positions.end()) return;

        auto& pos = it->second;
        int level = sig.tp_level();

        // 부분 청산 비율 결정
        double close_ratio = 0.0;
        if (level == 1) {
            close_ratio = m_params.tp1_ratio;
        } else if (level == 2) {
            close_ratio = m_params.tp2_ratio;
        } else if (level == 3) {
            close_ratio = 1.0; // 나머지 전량
        }

        double close_qty;
        if (level == 3) {
            close_qty = pos.quantity; // 남은 전량
        } else {
            close_qty = pos.initial_qty * close_ratio;
            close_qty = std::min(close_qty, pos.quantity);
        }

        if (close_qty <= 0.0) return;

        // PnL 계산
        double pnl = calc_pnl(pos.side, pos.entry_price, sig.price, close_qty);
        double fee = close_qty * sig.price * m_params.fee_rate;
        pnl -= fee;

        m_balance += pnl;
        pos.quantity -= close_qty;

        // 거래 기록
        record_trade(pos, sig.price, close_qty, pnl, sig.alert);

        // 잔량 0이면 포지션 제거
        if (pos.quantity <= 1e-12) {
            m_positions.erase(it);
        }

        update_equity();
    }

    void handle_sl(const BtSignal& sig) {
        close_position(sig.symbol, sig.price, "SL");
    }

    void handle_reentry(const BtSignal& sig) {
        // 기존 포지션 없으면 재진입 (같은 방향으로)
        if (m_positions.find(sig.symbol) != m_positions.end()) return;

        // 방향 결정: direction 기반
        BtSignal entry_sig = sig;
        entry_sig.alert = (sig.direction == "bull") ? "buy" : "sell";
        handle_entry(entry_sig);
    }

    void close_position(const std::string& symbol, double exit_price,
                        const std::string& reason) {
        auto it = m_positions.find(symbol);
        if (it == m_positions.end()) return;

        auto& pos = it->second;
        double pnl = calc_pnl(pos.side, pos.entry_price, exit_price, pos.quantity);
        double fee = pos.quantity * exit_price * m_params.fee_rate;
        pnl -= fee;

        m_balance += pnl;
        record_trade(pos, exit_price, pos.quantity, pnl, reason);
        m_positions.erase(it);
        update_equity();
    }

    void close_all_remaining() {
        // 미청산 포지션은 마지막 entry_price로 청산 (보수적)
        std::vector<std::string> symbols;
        for (auto& [sym, pos] : m_positions) {
            symbols.push_back(sym);
        }
        for (auto& sym : symbols) {
            auto& pos = m_positions[sym];
            close_position(sym, pos.entry_price, "EXPIRE");
        }
    }

    double calc_pnl(const std::string& side, double entry, double exit, double qty) const {
        if (side == "long") {
            return (exit - entry) * qty;
        } else {
            return (entry - exit) * qty;
        }
    }

    void record_trade(const BtPosition& pos, double exit_price, double qty,
                      double pnl, const std::string& reason) {
        BtTradeResult r;
        r.symbol      = pos.symbol;
        r.side        = pos.side;
        r.entry_price = pos.entry_price;
        r.exit_price  = exit_price;
        r.quantity    = qty;
        r.pnl         = pnl;
        r.pnl_pct     = (pos.entry_price > 0) ? (pnl / (qty * pos.entry_price)) * 100.0 : 0.0;
        r.exit_reason = reason;
        m_results.push_back(std::move(r));
    }

    void update_equity() {
        m_equity_curve.push_back(m_balance);
        if (m_balance > m_peak_balance) {
            m_peak_balance = m_balance;
        }
        double dd = (m_peak_balance > 0) ?
            (m_peak_balance - m_balance) / m_peak_balance * 100.0 : 0.0;
        m_max_dd = std::max(m_max_dd, dd);
    }

    BtMetrics compute_metrics() const {
        BtMetrics m;
        m.total_trades = static_cast<int>(m_results.size());
        m.final_balance = m_balance;
        m.total_pnl = m_balance - m_params.initial_balance;
        m.total_return_pct = (m_params.initial_balance > 0) ?
            (m.total_pnl / m_params.initial_balance) * 100.0 : 0.0;
        m.max_drawdown_pct = m_max_dd;

        double sum_wins = 0.0, sum_losses = 0.0;
        std::vector<double> returns;

        for (auto& r : m_results) {
            if (r.pnl > 0) {
                m.wins++;
                sum_wins += r.pnl;
            } else if (r.pnl < 0) {
                m.losses++;
                sum_losses += std::abs(r.pnl);
            }
            returns.push_back(r.pnl);
        }

        m.win_rate = (m.total_trades > 0) ?
            static_cast<double>(m.wins) / m.total_trades : 0.0;
        m.avg_pnl = (m.total_trades > 0) ? m.total_pnl / m.total_trades : 0.0;
        m.avg_win = (m.wins > 0) ? sum_wins / m.wins : 0.0;
        m.avg_loss = (m.losses > 0) ? sum_losses / m.losses : 0.0;
        m.profit_factor = (sum_losses > 0) ? sum_wins / sum_losses : 999.0;

        // Sharpe Ratio (일별이 아닌 거래별 - 간이 계산)
        if (returns.size() > 1) {
            double mean = std::accumulate(returns.begin(), returns.end(), 0.0) / returns.size();
            double sq_sum = 0.0;
            for (auto r : returns) {
                sq_sum += (r - mean) * (r - mean);
            }
            double stdev = std::sqrt(sq_sum / (returns.size() - 1));
            m.sharpe_ratio = (stdev > 1e-10) ? (mean / stdev) * std::sqrt(252.0) : 0.0;
        }

        return m;
    }

    BtParams m_params;
    double m_balance{0.0};
    double m_peak_balance{0.0};
    double m_max_dd{0.0};
    std::unordered_map<std::string, BtPosition> m_positions;
    std::vector<BtTradeResult> m_results;
    std::vector<double> m_equity_curve;
};

} // namespace hft::bt

// ============================================================================
// execution/trade_recorder.hpp -- Trade recording and statistics
// 2026-03-16 | Extracted from ExecutionEngine
//
// Responsibilities:
//   - m_trades vector: add_trade(), snapshot, get_stats()
//   - Trade capping (max kMaxTrades)
//   - PnL calculation helpers (static)
//   - Balance + PnL update on trade close
// ============================================================================
#pragma once

#include <mutex>
#include <string>
#include <vector>
#include <cmath>
#include <optional>
#include <algorithm>
#include <unordered_map>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "risk/symbol_scorer.hpp"      // TradeRecord
#include "risk/portfolio_risk.hpp"     // ManagedPosition
#include "risk/symbol_learner.hpp"     // SymbolLearner
#include "core/alert_manager.hpp"      // AlertManager

namespace hft {

class TradeRecorder {
public:
    // Takes references to shared state owned by ExecutionEngine
    TradeRecorder(std::mutex& pos_mtx,
                  double& balance,
                  double& peak_balance,
                  std::vector<TradeRecord>& trades,
                  AlertManager& alerts,
                  SymbolLearner& learner)
        : m_pos_mtx(pos_mtx)
        , m_balance(balance)
        , m_peak_balance(peak_balance)
        , m_trades(trades)
        , m_alerts(alerts)
        , m_learner(learner) {}

    // ── PnL calculation helpers (static) ──

    [[nodiscard]] static double calc_pnl(const std::string& side, double entry, double exit,
                                          double qty, int leverage) {
        if (entry <= 0 || qty <= 0) return 0.0;
        double direction = (side == "long") ? 1.0 : -1.0;
        return direction * (exit - entry) * qty;
    }

    [[nodiscard]] static double calc_fee(double price, double qty) {
        constexpr double TAKER_FEE_PCT = 0.0006;  // 0.06%
        double notional = price * qty;
        return notional * TAKER_FEE_PCT * 2.0;  // round-trip
    }

    // ── Record a completed trade ──
    // Caller must hold m_pos_mtx

    void record_trade(const std::string& symbol, const std::string& timeframe,
                      const std::string& exit_reason, double entry_price,
                      double exit_price, double quantity, double pnl, double fee,
                      const std::string& strategy = "unknown") {
        TradeRecord tr;
        tr.symbol      = symbol;
        tr.timeframe   = timeframe;
        tr.exit_reason = exit_reason;
        tr.entry_price = entry_price;
        tr.exit_price  = exit_price;
        tr.quantity    = quantity;
        tr.pnl         = pnl;
        tr.fee         = fee;
        tr.strategy    = strategy;
        m_trades.push_back(tr);
        m_learner.record_trade(tr);
        cap_trades();

        // Update balance
        m_balance += pnl;
        if (m_balance > m_peak_balance) m_peak_balance = m_balance;
    }

    // ── Record trade from a closing position ──
    // Computes PnL automatically. Caller must hold m_pos_mtx.

    double record_close(const ManagedPosition& pos, double exit_price,
                        const std::string& exit_reason, double close_qty = 0) {
        if (close_qty <= 0) close_qty = pos.quantity;
        double fee = calc_fee(exit_price, close_qty);
        double pnl = calc_pnl(pos.side, pos.entry_price, exit_price, close_qty, pos.leverage) - fee;

        record_trade(pos.symbol, pos.timeframe, exit_reason,
                     pos.entry_price, exit_price, close_qty, pnl, fee, pos.strategy);
        return pnl;
    }

    // ── Record trade from WS close with optional exchange-reported PnL ──
    // Caller must hold m_pos_mtx.

    double record_ws_close(const ManagedPosition& pos, double exit_price,
                           double realized_pnl) {
        double fee = calc_fee(exit_price, pos.quantity);
        double pnl = calc_pnl(pos.side, pos.entry_price, exit_price, pos.quantity, pos.leverage) - fee;

        // Use exchange-reported PnL if available
        if (std::abs(realized_pnl) > 0.0001) {
            pnl = realized_pnl - fee;
        }

        record_trade(pos.symbol, pos.timeframe, "WS_CLOSE",
                     pos.entry_price, exit_price, pos.quantity, pnl, fee, pos.strategy);
        return pnl;
    }

    // ── Thread-safe snapshot ──

    [[nodiscard]] std::vector<TradeRecord> snapshot() const {
        std::lock_guard lock(m_pos_mtx);
        return m_trades;
    }

    // ── Thread-safe stats ──

    [[nodiscard]] nlohmann::json get_stats(
            size_t open_positions,
            uint64_t orders_executed,
            uint64_t risk_skips,
            bool shadow_mode) const {
        std::lock_guard lock(m_pos_mtx);
        int total_trades = static_cast<int>(m_trades.size());
        int wins = 0;
        double total_pnl = 0;
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
            {"open_positions", open_positions},
            {"orders_executed", orders_executed},
            {"risk_skips", risk_skips},
            {"shadow_mode", shadow_mode}
        };
    }

    // ── Strategy performance stats (thread-safe) ──

    [[nodiscard]] nlohmann::json get_strategy_stats() const {
        std::lock_guard lock(m_pos_mtx);
        struct StratStats {
            int total{0};
            int wins{0};
            double total_pnl{0.0};
            double total_fee{0.0};
        };
        std::unordered_map<std::string, StratStats> by_strategy;

        for (auto& t : m_trades) {
            auto& s = by_strategy[t.strategy];
            s.total++;
            if (t.pnl > 0) s.wins++;
            s.total_pnl += t.pnl;
            s.total_fee += t.fee;
        }

        auto arr = nlohmann::json::array();
        for (auto& [name, s] : by_strategy) {
            double wr = s.total > 0 ? std::round(static_cast<double>(s.wins) / s.total * 10000.0) / 100.0 : 0.0;
            arr.push_back(nlohmann::json{
                {"strategy", name},
                {"total_trades", s.total},
                {"wins", s.wins},
                {"losses", s.total - s.wins},
                {"win_rate", wr},
                {"total_pnl", std::round(s.total_pnl * 10000.0) / 10000.0},
                {"total_fee", std::round(s.total_fee * 10000.0) / 10000.0},
                {"avg_pnl", s.total > 0 ? std::round(s.total_pnl / s.total * 10000.0) / 10000.0 : 0.0}
            });
        }

        // Sort by total_pnl ascending (worst strategies first)
        std::sort(arr.begin(), arr.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
            return a["total_pnl"].get<double>() < b["total_pnl"].get<double>();
        });

        return arr;
    }

    // ── Alert helpers ──

    void alert_trade(const std::string& level, const std::string& msg,
                     const std::string& symbol) {
        if (level == "info")
            m_alerts.info("TRADE", msg, symbol);
        else if (level == "warn")
            m_alerts.warn("TRADE", msg, symbol);
        else if (level == "critical")
            m_alerts.critical("TRADE", msg, symbol);
    }

    static constexpr size_t kMaxTrades = 2000;

private:
    void cap_trades() {
        if (m_trades.size() > kMaxTrades) {
            m_trades.erase(m_trades.begin(),
                           m_trades.begin() + static_cast<ptrdiff_t>(m_trades.size() - kMaxTrades));
        }
    }

    std::mutex& m_pos_mtx;
    double& m_balance;
    double& m_peak_balance;
    std::vector<TradeRecord>& m_trades;
    AlertManager& m_alerts;
    SymbolLearner& m_learner;
};

} // namespace hft

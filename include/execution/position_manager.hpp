// ============================================================================
// execution/position_manager.hpp -- Position lifecycle management
// 2026-03-16 | Extracted from ExecutionEngine
//
// Responsibilities:
//   - m_positions map: add, remove, find, snapshot
//   - Position sync with exchange (ghost detection + reverse import)
//   - Balance fetch from exchange
//   - ManagedPosition creation
// ============================================================================
#pragma once

#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <chrono>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

#include "risk/portfolio_risk.hpp"
#include "exchange/bitget_rest.hpp"
#include "core/state_persistence.hpp"

namespace hft {

class PositionManager {
public:
    // Takes references to shared state owned by ExecutionEngine
    PositionManager(std::mutex& pos_mtx,
                    double& balance,
                    double& peak_balance,
                    std::unordered_map<std::string, ManagedPosition>& positions,
                    PortfolioRiskManager& port_risk,
                    StatePersistence& state)
        : m_pos_mtx(pos_mtx)
        , m_balance(balance)
        , m_peak_balance(peak_balance)
        , m_positions(positions)
        , m_port_risk(port_risk)
        , m_state(state) {}

    // ── Snapshot (thread-safe) ──

    [[nodiscard]] std::unordered_map<std::string, ManagedPosition> snapshot() const {
        std::lock_guard lock(m_pos_mtx);
        return m_positions;
    }

    // ── Find matching position by symbol + side ──

    struct MatchResult {
        std::string key;
        double quantity{0.0};
    };

    [[nodiscard]] MatchResult find_by_symbol_side(const std::string& symbol,
                                                   const std::string& side) const {
        std::lock_guard lock(m_pos_mtx);
        for (auto& [key, pos] : m_positions) {
            if (pos.symbol == symbol && pos.side == side) {
                return {key, pos.quantity};
            }
        }
        return {};
    }

    // ── Register a new position ──

    void register_position(const std::string& symbol, const std::string& timeframe,
                          const std::string& side, double entry_price, double quantity,
                          int leverage, double sl_price, double tp1_price,
                          const std::string& tier, uint64_t oid,
                          const std::string& strategy = "unknown",
                          bool is_real = false) {
        std::lock_guard lock(m_pos_mtx);
        std::string key = symbol + "_" + timeframe + "_" + std::to_string(oid);
        ManagedPosition pos;
        pos.symbol      = symbol;
        pos.timeframe   = timeframe;
        pos.side        = side;
        pos.entry_price = entry_price;
        pos.quantity    = quantity;
        pos.leverage    = leverage;
        pos.sl_price    = sl_price;
        pos.tp1_price   = tp1_price;
        pos.tier        = tier;
        pos.strategy    = strategy;
        pos.opened_at   = std::chrono::system_clock::now();
        pos.is_real     = is_real;
        m_positions[key] = pos;
    }

    // ── Remove position by key (returns removed position if found) ──

    std::optional<ManagedPosition> remove(const std::string& key) {
        // Caller must hold m_pos_mtx
        auto it = m_positions.find(key);
        if (it == m_positions.end()) return std::nullopt;
        auto pos = std::move(it->second);
        m_positions.erase(it);
        return pos;
    }

    // ── Handle WS position close: find by symbol+side, remove, return position ──

    struct WsCloseResult {
        bool found{false};
        ManagedPosition pos;
        std::string key;
    };

    WsCloseResult find_and_prepare_ws_close(const std::string& symbol,
                                             const std::string& hold_side) {
        // Caller must hold m_pos_mtx
        for (auto it = m_positions.begin(); it != m_positions.end(); ++it) {
            if (it->second.symbol == symbol && it->second.side == hold_side) {
                WsCloseResult result;
                result.found = true;
                result.pos = it->second;
                result.key = it->first;
                m_positions.erase(it);
                return result;
            }
        }
        return {};
    }

    // ── Update position quantity (partial close) ──

    void reduce_quantity(const std::string& key, double amount) {
        // Caller must hold m_pos_mtx
        auto it = m_positions.find(key);
        if (it != m_positions.end()) {
            it->second.quantity -= amount;
            if (it->second.quantity <= 0) {
                m_positions.erase(it);
            }
        }
    }

    // ── Get position by key (caller holds lock) ──

    ManagedPosition* get(const std::string& key) {
        auto it = m_positions.find(key);
        return it != m_positions.end() ? &it->second : nullptr;
    }

    // ── Balance management ──

    void update_balance(double pnl) {
        // Caller must hold m_pos_mtx
        m_balance += pnl;
        if (m_balance > m_peak_balance) m_peak_balance = m_balance;
    }

    // ── Calculate used margin across all open positions ──

    double calc_used_margin(int default_leverage) const {
        // Caller must hold m_pos_mtx
        double used = 0.0;
        for (auto& [key, pos] : m_positions) {
            double notional = pos.entry_price * pos.quantity;
            int lev = pos.leverage > 0 ? pos.leverage : default_leverage;
            used += notional / lev;
        }
        return used;
    }

    // ── Exchange sync: fetch real balance ──

    struct BalanceResult {
        double available{0.0};
        double equity{0.0};
        double unrealized_pnl{0.0};
        bool ok{false};
    };

    BalanceResult fetch_real_balance(BitgetRestClient& rest) {
        BalanceResult result;
        try {
            auto account = rest.get_account();
            auto code = account.value("code", "99999");
            if (code == "00000" && account.contains("data")) {
                auto& data = account["data"];
                double available = 0.0;
                double equity = 0.0;
                double uPnL = 0.0;
                if (data.contains("available")) {
                    available = std::stod(data["available"].get<std::string>());
                }
                if (data.contains("accountEquity")) {
                    equity = std::stod(data["accountEquity"].get<std::string>());
                }
                if (data.contains("unrealizedPL")) {
                    uPnL = std::stod(data["unrealizedPL"].get<std::string>());
                }

                result.available = available;
                result.equity = equity;
                result.unrealized_pnl = uPnL;

                // available이 유효할 때만 m_balance 업데이트
                // available=0이면 기존 WS 값 유지 (equity fallback 방지)
                if (available > 0 || equity > 0) {
                    std::lock_guard lock(m_pos_mtx);
                    if (available > 0) {
                        m_balance = available;  // 가용잔고만 balance에 세팅
                    }
                    // available=0이면 m_balance 유지 (WS에서 세팅한 값)
                    double real_equity = (equity > 0) ? equity : m_balance;
                    if (m_peak_balance > real_equity * 2.0 || m_peak_balance < m_balance) {
                        m_peak_balance = real_equity;
                        spdlog::info("[Exec] Peak balance corrected to {:.2f}", m_peak_balance);
                    }
                    m_port_risk.update_balance(m_balance);
                    result.ok = true;
                    spdlog::info("[Exec] Real Bitget balance: available={:.2f} equity={:.2f} uPnL={:.2f}",
                        available, equity, uPnL);
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
        return result;
    }

    // ── Exchange sync: bidirectional position reconciliation ──
    // 1) Ghost removal: internal positions not on exchange
    // 2) Reverse import: exchange positions not tracked internally

    void sync_positions_with_exchange(BitgetRestClient& rest,
                                       std::atomic<uint64_t>& orders_executed) {
        std::lock_guard lock(m_pos_mtx);

        try {
            auto resp = rest.get_positions();
            auto code = resp.value("code", "99999");
            if (code != "00000") {
                spdlog::error("[Sync] Failed to fetch exchange positions: code={} msg={}",
                    code, resp.value("msg", "unknown"));
                return;
            }

            struct ExchangePos {
                std::string symbol;
                std::string hold_side;
                double total{0.0};
                double avg_price{0.0};
                int leverage{10};
            };
            std::unordered_map<std::string, ExchangePos> exchange_positions;

            auto parse_double = [](const nlohmann::json& j, const std::string& key) -> double {
                if (!j.contains(key)) return 0.0;
                auto& v = j[key];
                if (v.is_string()) {
                    try { return std::stod(v.get<std::string>()); } catch (...) { return 0.0; }
                } else if (v.is_number()) {
                    return v.get<double>();
                }
                return 0.0;
            };
            auto parse_int = [](const nlohmann::json& j, const std::string& key, int def = 10) -> int {
                if (!j.contains(key)) return def;
                auto& v = j[key];
                if (v.is_string()) {
                    try { return std::stoi(v.get<std::string>()); } catch (...) { return def; }
                } else if (v.is_number()) {
                    return v.get<int>();
                }
                return def;
            };

            if (resp.contains("data") && resp["data"].is_array()) {
                for (auto& pos : resp["data"]) {
                    ExchangePos ep;
                    ep.symbol    = pos.value("symbol", "");
                    ep.hold_side = pos.value("holdSide", "");
                    ep.total     = parse_double(pos, "total");
                    ep.avg_price = parse_double(pos, "averageOpenPrice");
                    ep.leverage  = parse_int(pos, "leverage", 10);

                    if (!ep.symbol.empty() && ep.total > 0) {
                        std::string key = ep.symbol + ":" + ep.hold_side;
                        exchange_positions[key] = ep;
                    }
                }
            }

            spdlog::info("[Sync] Exchange has {} open position(s), internal has {}",
                exchange_positions.size(), m_positions.size());

            // === 0) Mark positions matching exchange as is_real ===
            for (auto& [key, pos] : m_positions) {
                std::string lookup = pos.symbol + ":" + pos.side;
                if (exchange_positions.find(lookup) != exchange_positions.end()) {
                    if (!pos.is_real) {
                        pos.is_real = true;
                        spdlog::info("[Sync] Marked as real: {} {} (exists on exchange)", pos.symbol, pos.side);
                    }
                }
            }

            // === 1) Forward: Remove ghost positions (only real ones) ===
            std::vector<std::string> to_remove;
            for (auto& [key, pos] : m_positions) {
                std::string lookup = pos.symbol + ":" + pos.side;
                if (exchange_positions.find(lookup) == exchange_positions.end()) {
                    if (pos.is_real) {
                        // Real position gone from exchange — closed externally (SL/liquidation)
                        to_remove.push_back(key);
                        spdlog::warn("[Sync] Ghost position removed: key={} symbol={} side={} entry={:.2f} qty={:.6f}",
                            key, pos.symbol, pos.side, pos.entry_price, pos.quantity);
                    }
                    // Paper/shadow positions: not on exchange is NORMAL, skip
                }
            }
            for (auto& key : to_remove) {
                auto it = m_positions.find(key);
                if (it != m_positions.end()) {
                    spdlog::warn("[Sync] Ghost PnL not recorded (exchange closed): {} {} entry={:.4f} qty={:.6f}",
                        it->second.symbol, it->second.side, it->second.entry_price, it->second.quantity);
                    m_positions.erase(it);
                }
            }

            // === 2) Reverse: Import exchange positions not in internal ===
            std::unordered_set<std::string> internal_lookup;
            for (auto& [key, pos] : m_positions) {
                internal_lookup.insert(pos.symbol + ":" + pos.side);
            }

            int imported = 0;
            for (auto& [exkey, ep] : exchange_positions) {
                if (internal_lookup.find(exkey) == internal_lookup.end()) {
                    ManagedPosition mp;
                    mp.symbol      = ep.symbol;
                    mp.timeframe   = "ext";
                    mp.side        = ep.hold_side;
                    mp.entry_price = ep.avg_price;
                    mp.quantity    = ep.total;
                    mp.leverage    = ep.leverage;
                    mp.tier        = "C";
                    mp.is_real     = true;  // 거래소에서 온 포지션은 항상 real
                    mp.opened_at   = std::chrono::system_clock::now();

                    std::string internal_key = ep.symbol + "_" + ep.hold_side + "_ext";
                    m_positions[internal_key] = mp;
                    imported++;

                    spdlog::info("[Sync] Imported exchange position: {} {} entry={:.4f} qty={:.6f} lev={}x",
                        ep.symbol, ep.hold_side, ep.avg_price, ep.total, ep.leverage);
                }
            }

            bool changed = !to_remove.empty() || imported > 0;
            if (changed) {
                spdlog::info("[Sync] Reconciliation: removed={} imported={} total={}",
                    to_remove.size(), imported, m_positions.size());
                m_state.save_state(m_balance, m_peak_balance, m_positions, m_trades_ref ? *m_trades_ref : std::vector<TradeRecord>{}, orders_executed.load());
            } else {
                spdlog::info("[Sync] All {} position(s) in sync", m_positions.size());
            }

        } catch (const std::exception& e) {
            spdlog::error("[Sync] Position sync error: {}", e.what());
        }
    }

    // Set trade vector reference for save_state calls during sync
    void set_trades_ref(std::vector<TradeRecord>* trades) { m_trades_ref = trades; }

    // ── Save state helper (caller must hold m_pos_mtx) ──

    void save_state(const std::vector<TradeRecord>& trades, uint64_t orders_executed) {
        m_state.save_state(m_balance, m_peak_balance, m_positions, trades, orders_executed);
    }

private:
    std::mutex& m_pos_mtx;
    double& m_balance;
    double& m_peak_balance;
    std::unordered_map<std::string, ManagedPosition>& m_positions;
    PortfolioRiskManager& m_port_risk;
    StatePersistence& m_state;
    std::vector<TradeRecord>* m_trades_ref{nullptr};
};

} // namespace hft

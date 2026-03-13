// ============================================================================
// core/state_persistence.hpp -- JSON 상태 영속화
// v1.0 | 2026-03-13
//
// 원자적 쓰기 (tmp → rename), 30초 자동 저장, 포지션 변경시 즉시 저장
// 서버 재시작 시 자동 복구 (positions, trades, balance, peak_balance)
// ============================================================================
#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <fstream>
#include <filesystem>
#include <chrono>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "risk/portfolio_risk.hpp"
#include "risk/symbol_scorer.hpp"

namespace hft {

class StatePersistence {
public:
    explicit StatePersistence(const std::string& data_dir = "data")
        : m_data_dir(data_dir)
    {
        std::filesystem::create_directories(data_dir);
    }

    // ── State Save/Load ──

    void save_state(
        double balance, double peak_balance,
        const std::unordered_map<std::string, ManagedPosition>& positions,
        const std::vector<TradeRecord>& trades)
    {
        std::lock_guard lock(m_mtx);
        try {
            nlohmann::json state;
            state["balance"] = balance;
            state["peak_balance"] = peak_balance;
            state["saved_at"] = iso_now();

            // Positions
            auto& pos_arr = state["positions"];
            pos_arr = nlohmann::json::object();
            for (auto& [key, p] : positions) {
                pos_arr[key] = nlohmann::json{
                    {"symbol", p.symbol}, {"timeframe", p.timeframe},
                    {"side", p.side}, {"entry_price", p.entry_price},
                    {"quantity", p.quantity}, {"leverage", p.leverage},
                    {"sl_price", p.sl_price}, {"tp1_price", p.tp1_price},
                    {"tier", p.tier}
                };
            }

            // Trades (last 2000)
            auto& trades_arr = state["trades"];
            trades_arr = nlohmann::json::array();
            size_t start = trades.size() > 2000 ? trades.size() - 2000 : 0;
            for (size_t i = start; i < trades.size(); i++) {
                auto& t = trades[i];
                trades_arr.push_back(nlohmann::json{
                    {"symbol", t.symbol}, {"timeframe", t.timeframe},
                    {"exit_reason", t.exit_reason},
                    {"pnl", t.pnl}, {"fee", t.fee}
                });
            }

            atomic_write(m_data_dir + "/state.json", state);
            m_last_save = std::chrono::steady_clock::now();
        } catch (const std::exception& e) {
            spdlog::error("[STATE] Save failed: {}", e.what());
        }
    }

    struct LoadedState {
        double balance{0.0};
        double peak_balance{0.0};
        std::unordered_map<std::string, ManagedPosition> positions;
        std::vector<TradeRecord> trades;
        bool valid{false};
    };

    [[nodiscard]] LoadedState load_state() {
        std::lock_guard lock(m_mtx);
        LoadedState result;
        auto path = m_data_dir + "/state.json";

        if (!std::filesystem::exists(path)) {
            spdlog::info("[STATE] No state file found, starting fresh");
            return result;
        }

        try {
            std::ifstream f(path);
            auto state = nlohmann::json::parse(f);

            result.balance = state.value("balance", 0.0);
            result.peak_balance = state.value("peak_balance", 0.0);

            if (state.contains("positions")) {
                for (auto& [key, pj] : state["positions"].items()) {
                    ManagedPosition p;
                    p.symbol      = pj.value("symbol", "");
                    p.timeframe   = pj.value("timeframe", "");
                    p.side        = pj.value("side", "");
                    p.entry_price = pj.value("entry_price", 0.0);
                    p.quantity    = pj.value("quantity", 0.0);
                    p.leverage    = pj.value("leverage", 10);
                    p.sl_price    = pj.value("sl_price", 0.0);
                    p.tp1_price   = pj.value("tp1_price", 0.0);
                    p.tier        = pj.value("tier", "C");
                    result.positions[key] = p;
                }
            }

            if (state.contains("trades")) {
                for (auto& tj : state["trades"]) {
                    result.trades.push_back(tj.get<TradeRecord>());
                }
            }

            result.valid = true;
            spdlog::info("[STATE] Loaded: balance={:.2f} positions={} trades={}",
                result.balance, result.positions.size(), result.trades.size());
        } catch (const std::exception& e) {
            spdlog::error("[STATE] Load failed: {}", e.what());
        }
        return result;
    }

    // ── Auto-save Check ──

    [[nodiscard]] bool needs_save(int interval_sec = 30) const {
        auto elapsed = std::chrono::steady_clock::now() - m_last_save;
        return elapsed >= std::chrono::seconds(interval_sec);
    }

private:
    void atomic_write(const std::string& path, const nlohmann::json& data) {
        auto tmp = path + ".tmp";
        {
            std::ofstream f(tmp);
            if (!f.is_open()) throw std::runtime_error("Cannot open " + tmp);
            f << data.dump(2);
        }
        std::filesystem::rename(tmp, path);
    }

    static std::string iso_now() {
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

    std::mutex m_mtx;
    std::string m_data_dir;
    std::chrono::steady_clock::time_point m_last_save{std::chrono::steady_clock::now()};
};

} // namespace hft

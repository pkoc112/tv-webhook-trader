// ============================================================================
// core/state_persistence.hpp -- JSON 상태 영속화 v2.0
// v2.0 | 2026-03-15 | fsync 보장 + corruption recovery + backup
//
// 원자적 쓰기 (tmp → fsync → rename), 30초 자동 저장, 포지션 변경시 즉시 저장
// 서버 재시작 시 자동 복구: main → backup → 초기화 순서
// ============================================================================
#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <cstdio>  // fflush, fileno

#ifdef __linux__
#include <unistd.h>  // fsync
#endif

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
        auto path = m_data_dir + "/state.json";
        auto backup = m_data_dir + "/state.json.bak";

        // 우선순위: main → backup → 초기화
        LoadedState result = try_load_file(path);
        if (result.valid) return result;

        spdlog::warn("[STATE] Main state corrupt/missing, trying backup...");
        result = try_load_file(backup);
        if (result.valid) {
            spdlog::info("[STATE] Recovered from backup file");
            return result;
        }

        spdlog::info("[STATE] No valid state found, starting fresh");
        return result;
    }

    // ── Auto-save Check ──

    [[nodiscard]] bool needs_save(int interval_sec = 30) const {
        auto elapsed = std::chrono::steady_clock::now() - m_last_save;
        return elapsed >= std::chrono::seconds(interval_sec);
    }

private:
    // 파일에서 상태 로드 시도 (corruption 감지)
    LoadedState try_load_file(const std::string& path) {
        LoadedState result;
        if (!std::filesystem::exists(path)) return result;

        try {
            std::ifstream f(path);
            if (!f.is_open()) return result;

            auto state = nlohmann::json::parse(f);

            // 최소 유효성 검증
            if (!state.contains("balance") || !state.contains("positions")) {
                spdlog::warn("[STATE] File {} missing required fields", path);
                return result;
            }

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
            spdlog::info("[STATE] Loaded {}: balance={:.2f} positions={} trades={}",
                path, result.balance, result.positions.size(), result.trades.size());
        } catch (const std::exception& e) {
            spdlog::error("[STATE] Load failed for {}: {}", path, e.what());
        }
        return result;
    }

    void atomic_write(const std::string& path, const nlohmann::json& data) {
        auto tmp = path + ".tmp";
        auto backup = path + ".bak";

        // 1. Write to temp file
        {
            std::FILE* fp = std::fopen(tmp.c_str(), "w");
            if (!fp) throw std::runtime_error("Cannot open " + tmp);
            auto content = data.dump(2);
            std::fwrite(content.data(), 1, content.size(), fp);
            std::fflush(fp);
#ifdef __linux__
            // fsync: 디스크에 물리적으로 기록 보장
            ::fsync(fileno(fp));
#endif
            std::fclose(fp);
        }

        // 2. Validate written file (re-read and parse)
        {
            std::ifstream check(tmp);
            if (!check.is_open()) throw std::runtime_error("Cannot verify " + tmp);
            auto verify = nlohmann::json::parse(check);
            if (!verify.contains("balance")) {
                throw std::runtime_error("Written state file validation failed");
            }
        }

        // 3. Backup current state before overwrite
        if (std::filesystem::exists(path)) {
            std::error_code ec;
            std::filesystem::copy_file(path, backup,
                std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) {
                spdlog::warn("[STATE] Backup copy failed: {}", ec.message());
            }
        }

        // 4. Atomic rename
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

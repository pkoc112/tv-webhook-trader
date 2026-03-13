// ============================================================================
// backtest/log_parser.hpp -- sfx-trader state.json 파서
// v1.0 | 2026-03-13
//
// sfx-trader의 signals 배열을 파싱하여 백테스트용 시그널 시퀀스로 변환
// ============================================================================
#pragma once

#include <string>
#include <vector>
#include <set>
#include <fstream>
#include <algorithm>
#include <stdexcept>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace hft::bt {

using json = nlohmann::json;

// -- 백테스트용 시그널 레코드 --
struct BtSignal {
    std::string time;           // ISO 8601
    std::string alert;          // buy, sell, TP1, TP2, TP3, sl, re-entry
    std::string symbol;
    double      price{0.0};
    double      entry_price{0.0};
    std::string direction;      // bull / bear
    int         rating{0};      // 0, 1, 2
    std::string timeframe;      // "1", "5", "15", "60", ...

    // 진입 시그널인지 여부
    [[nodiscard]] bool is_entry() const {
        return alert == "buy" || alert == "sell";
    }

    // TP 시그널인지
    [[nodiscard]] bool is_tp() const {
        return alert == "TP1" || alert == "TP2" || alert == "TP3";
    }

    // SL 시그널인지
    [[nodiscard]] bool is_sl() const {
        return alert == "sl" || alert == "SL";
    }

    // 재진입인지
    [[nodiscard]] bool is_reentry() const {
        return alert == "re-entry" || alert == "RE";
    }

    // TP 레벨 번호 (1, 2, 3 또는 0)
    [[nodiscard]] int tp_level() const {
        if (alert == "TP1") return 1;
        if (alert == "TP2") return 2;
        if (alert == "TP3") return 3;
        return 0;
    }
};

// -- 백테스트용 완료된 거래 레코드 --
struct BtTrade {
    std::string timestamp;
    std::string symbol;
    std::string side;           // long / short
    double      entry_price{0.0};
    double      exit_price{0.0};
    double      quantity{0.0};
    double      pnl{0.0};
    double      pnl_pct{0.0};
    double      fee{0.0};
    double      duration_min{0.0};
    std::string exit_reason;    // TP1, TP2, TP3, SL, buy, sell
    std::string timeframe;      // "1", "5", "15", "60", ...
};

// -- state.json 전체 파싱 결과 --
struct ParsedState {
    std::vector<BtSignal> signals;
    std::vector<BtTrade>  trades;
    double                balance{0.0};
    double                total_pnl{0.0};
    int                   total_trades{0};
    int                   wins{0};
    int                   losses{0};
};

class LogParser {
public:
    // state.json 파일에서 시그널 + 거래 내역 로드
    static ParsedState load(const std::string& state_json_path) {
        std::ifstream f(state_json_path);
        if (!f.is_open()) {
            throw std::runtime_error("Cannot open: " + state_json_path);
        }

        auto j = json::parse(f);
        ParsedState result;

        // 계정 통계
        result.balance      = j.value("balance", 0.0);
        result.total_pnl    = j.value("total_pnl", 0.0);
        result.total_trades = j.value("total_trades", 0);
        result.wins         = j.value("wins", 0);
        result.losses       = j.value("losses", 0);

        // 시그널 파싱
        if (j.contains("signals") && j["signals"].is_array()) {
            for (auto& s : j["signals"]) {
                BtSignal sig;
                sig.time        = s.value("time", "");
                sig.alert       = s.value("alert", "");
                sig.symbol      = s.value("symbol", "");
                sig.price       = s.value("price", 0.0);
                sig.entry_price = s.value("entry_price", 0.0);
                sig.direction   = s.value("direction", "");
                sig.rating      = s.value("rating", 0);
                sig.timeframe   = s.value("timeframe", "");
                result.signals.push_back(std::move(sig));
            }
        }

        // 거래 내역 파싱 (검증용)
        if (j.contains("trades") && j["trades"].is_array()) {
            for (auto& t : j["trades"]) {
                BtTrade trade;
                trade.timestamp    = t.value("timestamp", "");
                trade.symbol       = t.value("symbol", "");
                trade.side         = t.value("side", "");
                trade.entry_price  = t.value("entry_price", 0.0);
                trade.exit_price   = t.value("exit_price", 0.0);
                trade.quantity     = t.value("quantity", 0.0);
                trade.pnl          = t.value("pnl", 0.0);
                trade.pnl_pct      = t.value("pnl_pct", 0.0);
                trade.fee          = t.value("fee", 0.0);
                trade.duration_min = t.value("duration_min", 0.0);
                trade.exit_reason  = t.value("exit_reason", "");
                trade.timeframe    = t.value("timeframe", "");
                result.trades.push_back(std::move(trade));
            }
        }

        spdlog::info("[LogParser] Loaded {} signals, {} trades from {}",
            result.signals.size(), result.trades.size(), state_json_path);

        return result;
    }

    // 심볼별 시그널 필터링
    static std::vector<BtSignal> filter_by_symbol(
            const std::vector<BtSignal>& signals, const std::string& symbol) {
        std::vector<BtSignal> out;
        std::copy_if(signals.begin(), signals.end(), std::back_inserter(out),
            [&](const BtSignal& s) { return s.symbol == symbol; });
        return out;
    }

    // 고유 심볼 목록 추출
    static std::vector<std::string> unique_symbols(const std::vector<BtSignal>& signals) {
        std::set<std::string> syms;
        for (auto& s : signals) {
            if (!s.symbol.empty()) syms.insert(s.symbol);
        }
        return {syms.begin(), syms.end()};
    }

    // 타임프레임별 시그널 필터링
    static std::vector<BtSignal> filter_by_timeframe(
            const std::vector<BtSignal>& signals, const std::string& tf) {
        std::vector<BtSignal> out;
        std::copy_if(signals.begin(), signals.end(), std::back_inserter(out),
            [&](const BtSignal& s) { return s.timeframe == tf; });
        return out;
    }

    // 고유 타임프레임 목록 추출
    static std::vector<std::string> unique_timeframes(const std::vector<BtSignal>& signals) {
        std::set<std::string> tfs;
        for (auto& s : signals) {
            if (!s.timeframe.empty()) tfs.insert(s.timeframe);
        }
        return {tfs.begin(), tfs.end()};
    }
};

} // namespace hft::bt

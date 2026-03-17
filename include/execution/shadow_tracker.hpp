// ============================================================================
// execution/shadow_tracker.hpp -- Shadow Learning: 모든 시그널 가상 추적
// 2026-03-17 | 실전 매매와 완전 분리된 학습 전용 가상 트래커
//
// 목적:
//   실전 리스크 필터 없이 모든 시그널을 무조건 가상 진입/청산하여
//   심볼×TF별 실적을 체점. 이 데이터를 기반으로 실전 매매 시
//   "이 심볼은 점수가 좋으니 진입해도 괜찮아" vs "무시해" 결정.
//
// 설계:
//   - process_signal() 최상단에서 호출 (어떤 필터보다 먼저)
//   - 자체 가상 포지션 맵 (실전과 완전 분리)
//   - 무제한 포지션 (한도 없음)
//   - 고정 노셔널(100 USDT)로 PnL 표준화
//   - 완료된 거래 → SymbolLearner에 기록 → 스코어링
//   - 영속화: data/shadow_positions.json
// ============================================================================
#pragma once

#include <mutex>
#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <fstream>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>

#include "webhook/signal_types.hpp"
#include "risk/symbol_scorer.hpp"    // TradeRecord
#include "risk/symbol_learner.hpp"   // SymbolLearner

namespace hft {

class ShadowTracker {
public:
    explicit ShadowTracker(SymbolLearner& learner,
                           const std::string& data_dir = "data")
        : m_learner(learner)
        , m_data_dir(data_dir)
    {
        load_state();
        spdlog::info("[SHADOW] Initialized: {} virtual positions, {} completed trades",
            m_positions.size(), m_total_closes);
    }

    // ================================================================
    // 메인 입력점: 모든 시그널을 여기로 보낸다 (필터 이전에 호출)
    // 선물 시그널만 처리 (현물은 별도)
    // ================================================================
    void track(const WebhookSignal& sig) {
        if (sig.market_type == "spot") return;  // 현물은 별도 관리
        if (sig.symbol.empty()) return;

        std::lock_guard lock(m_mtx);

        switch (sig.sig_type) {
            case SignalType::Entry:
            case SignalType::ReEntry:
                track_entry(sig);
                break;
            case SignalType::TP:
                track_tp(sig);
                break;
            case SignalType::SL:
                track_sl(sig);
                break;
            default:
                break;
        }
    }

    // ================================================================
    // 스냅샷 (대시보드용)
    // ================================================================

    struct ShadowStats {
        uint64_t total_entries{0};
        uint64_t total_closes{0};
        uint64_t open_positions{0};
        int wins{0};
        int losses{0};
        double total_pnl{0.0};
        double win_rate{0.0};
    };

    [[nodiscard]] ShadowStats get_stats() const {
        std::lock_guard lock(m_mtx);
        ShadowStats s;
        s.total_entries = m_total_entries;
        s.total_closes = m_total_closes;
        s.open_positions = m_positions.size();
        s.wins = m_wins;
        s.losses = m_losses;
        s.total_pnl = m_total_pnl;
        s.win_rate = m_total_closes > 0
            ? std::round(static_cast<double>(m_wins) / m_total_closes * 10000.0) / 100.0
            : 0.0;
        return s;
    }

    [[nodiscard]] nlohmann::json get_stats_json() const {
        auto s = get_stats();
        return nlohmann::json{
            {"total_entries", s.total_entries},
            {"total_closes", s.total_closes},
            {"open_positions", s.open_positions},
            {"wins", s.wins},
            {"losses", s.losses},
            {"total_pnl", std::round(s.total_pnl * 10000.0) / 10000.0},
            {"win_rate", s.win_rate}
        };
    }

    [[nodiscard]] nlohmann::json get_positions_json() const {
        std::lock_guard lock(m_mtx);
        auto arr = nlohmann::json::array();
        for (auto& [key, vp] : m_positions) {
            arr.push_back(nlohmann::json{
                {"key", key},
                {"symbol", vp.symbol},
                {"timeframe", vp.timeframe},
                {"side", vp.side},
                {"entry_price", vp.entry_price},
                {"strategy", vp.strategy},
                {"sl_price", vp.sl_price},
                {"tp1_price", vp.tp1_price},
                {"age_min", std::chrono::duration_cast<std::chrono::minutes>(
                    std::chrono::system_clock::now() - vp.opened_at).count()}
            });
        }
        return arr;
    }

    [[nodiscard]] nlohmann::json get_trades_json(size_t limit = 100) const {
        std::lock_guard lock(m_mtx);
        auto arr = nlohmann::json::array();
        size_t start = m_trades.size() > limit ? m_trades.size() - limit : 0;
        for (size_t i = start; i < m_trades.size(); i++) {
            auto& t = m_trades[i];
            arr.push_back(nlohmann::json{
                {"symbol", t.symbol},
                {"timeframe", t.timeframe},
                {"entry_price", t.entry_price},
                {"exit_price", t.exit_price},
                {"pnl", std::round(t.pnl * 10000.0) / 10000.0},
                {"fee", std::round(t.fee * 10000.0) / 10000.0},
                {"exit_reason", t.exit_reason},
                {"strategy", t.strategy}
            });
        }
        return arr;
    }

    // ================================================================
    // 심볼별 성적표 (대시보드용)
    // ================================================================
    [[nodiscard]] nlohmann::json get_symbol_report() const {
        std::lock_guard lock(m_mtx);
        struct SymStats {
            int total{0}, wins{0};
            double total_pnl{0.0};
        };
        std::unordered_map<std::string, SymStats> by_symbol;

        for (auto& t : m_trades) {
            auto& s = by_symbol[t.symbol];
            s.total++;
            if (t.pnl > 0) s.wins++;
            s.total_pnl += t.pnl;
        }

        auto arr = nlohmann::json::array();
        for (auto& [sym, s] : by_symbol) {
            double wr = s.total > 0
                ? std::round(static_cast<double>(s.wins) / s.total * 10000.0) / 100.0
                : 0.0;
            std::string grade;
            if (s.total < 5) grade = "?";        // 데이터 부족
            else if (wr >= 65 && s.total_pnl > 0) grade = "A+";
            else if (wr >= 55 && s.total_pnl > 0) grade = "A";
            else if (wr >= 50 && s.total_pnl > 0) grade = "B";
            else if (wr >= 45) grade = "C";
            else if (wr >= 35) grade = "D";
            else grade = "F";

            arr.push_back(nlohmann::json{
                {"symbol", sym},
                {"total", s.total},
                {"wins", s.wins},
                {"losses", s.total - s.wins},
                {"win_rate", wr},
                {"total_pnl", std::round(s.total_pnl * 10000.0) / 10000.0},
                {"avg_pnl", s.total > 0
                    ? std::round(s.total_pnl / s.total * 10000.0) / 10000.0
                    : 0.0},
                {"grade", grade}
            });
        }

        // Sort by total_pnl descending (best first)
        std::sort(arr.begin(), arr.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
            return a["total_pnl"].get<double>() > b["total_pnl"].get<double>();
        });

        return arr;
    }

    // ================================================================
    // 주기적 저장 (execution engine의 periodic_tasks에서 호출)
    // ================================================================
    void save_state() {
        std::lock_guard lock(m_mtx);
        save_state_locked();
    }

private:
    // ── 가상 포지션 구조체 ──
    struct VirtualPosition {
        std::string symbol;
        std::string timeframe;
        std::string side;          // "long" / "short"
        std::string strategy;
        double entry_price{0};
        double sl_price{0};
        double tp1_price{0};
        std::chrono::system_clock::time_point opened_at;
    };

    // ── 시그널 방향 → 포지션 사이드 매핑 ──
    static std::string dir_to_side(const WebhookSignal& sig) {
        // Entry: action="buy" → long, action="sell" → short
        // TP/SL: signal_direction="bull" → 원래 long이었음, "bear" → 원래 short
        if (sig.sig_type == SignalType::Entry || sig.sig_type == SignalType::ReEntry) {
            return (sig.action == "buy") ? "long" : "short";
        }
        // TP/SL은 원래 포지션의 방향
        return (sig.signal_direction == "bull") ? "long" : "short";
    }

    static std::string make_key(const std::string& symbol,
                                 const std::string& tf,
                                 const std::string& side) {
        return symbol + ":" + tf + ":" + side;
    }

    // ── Entry 처리: 무조건 가상 진입 ──
    void track_entry(const WebhookSignal& sig) {
        std::string side = dir_to_side(sig);
        std::string tf = sig.timeframe.empty() ? "0" : sig.timeframe;
        std::string key = make_key(sig.symbol, tf, side);

        // 이미 같은 포지션이 있으면 스킵 (중복 방지)
        if (m_positions.count(key)) {
            spdlog::debug("[SHADOW] Already have position: {}", key);
            return;
        }

        // 반대 포지션이 있으면 청산 후 반전 (reversal)
        std::string opp_side = (side == "long") ? "short" : "long";
        std::string opp_key = make_key(sig.symbol, tf, opp_side);
        auto opp_it = m_positions.find(opp_key);
        if (opp_it != m_positions.end()) {
            // 현재가로 반대 포지션 청산
            close_position(opp_it, sig.price, "REVERSAL");
        }

        VirtualPosition vp;
        vp.symbol      = sig.symbol;
        vp.timeframe   = tf;
        vp.side        = side;
        vp.strategy    = sig.strategy_name;
        vp.entry_price = sig.price;
        vp.sl_price    = sig.sl;
        vp.tp1_price   = sig.tp1;
        vp.opened_at   = std::chrono::system_clock::now();

        m_positions[key] = vp;
        m_total_entries++;

        spdlog::info("[SHADOW] ENTRY {} {} {} @ {:.6f} (total open: {})",
            vp.symbol, tf, side, vp.entry_price, m_positions.size());

        maybe_save();
    }

    // ── TP 처리 ──
    void track_tp(const WebhookSignal& sig) {
        std::string side = dir_to_side(sig);
        std::string tf = sig.timeframe.empty() ? "0" : sig.timeframe;
        std::string key = make_key(sig.symbol, tf, side);

        auto it = m_positions.find(key);
        if (it == m_positions.end()) {
            // TF가 안 맞을 수 있으므로 같은 symbol:side로 검색
            it = find_by_symbol_side(sig.symbol, side);
            if (it == m_positions.end()) {
                spdlog::debug("[SHADOW] TP no match: {}", key);
                return;
            }
        }

        close_position(it, sig.price, "TP");
    }

    // ── SL 처리 ──
    void track_sl(const WebhookSignal& sig) {
        std::string side = dir_to_side(sig);
        std::string tf = sig.timeframe.empty() ? "0" : sig.timeframe;
        std::string key = make_key(sig.symbol, tf, side);

        auto it = m_positions.find(key);
        if (it == m_positions.end()) {
            it = find_by_symbol_side(sig.symbol, side);
            if (it == m_positions.end()) {
                spdlog::debug("[SHADOW] SL no match: {}", key);
                return;
            }
        }

        close_position(it, sig.price, "SL");
    }

    // ── 같은 symbol+side로 가상 포지션 검색 (TF 무관) ──
    using PosIter = std::unordered_map<std::string, VirtualPosition>::iterator;

    PosIter find_by_symbol_side(const std::string& symbol, const std::string& side) {
        for (auto it = m_positions.begin(); it != m_positions.end(); ++it) {
            if (it->second.symbol == symbol && it->second.side == side) {
                return it;
            }
        }
        return m_positions.end();
    }

    // ── 가상 포지션 청산 + PnL 기록 ──
    void close_position(PosIter it, double exit_price, const std::string& reason) {
        auto& vp = it->second;

        // 고정 노셔널 100 USDT로 표준화
        constexpr double VIRTUAL_NOTIONAL = 100.0;
        double qty = (vp.entry_price > 0) ? VIRTUAL_NOTIONAL / vp.entry_price : 0;

        double direction = (vp.side == "long") ? 1.0 : -1.0;
        double pnl_raw = direction * (exit_price - vp.entry_price) * qty;

        // 수수료: 0.06% taker × 2 (round-trip)
        constexpr double TAKER_FEE = 0.0006;
        double fee = (vp.entry_price * qty + exit_price * qty) * TAKER_FEE;
        double pnl = pnl_raw - fee;

        // TradeRecord 생성
        TradeRecord tr;
        tr.symbol      = vp.symbol;
        tr.timeframe   = vp.timeframe;
        tr.exit_reason = "SHADOW_" + reason;
        tr.entry_price = vp.entry_price;
        tr.exit_price  = exit_price;
        tr.quantity     = qty;
        tr.pnl         = pnl;
        tr.fee         = fee;
        tr.strategy    = vp.strategy;

        // 통계 업데이트
        if (pnl > 0) m_wins++;
        else m_losses++;
        m_total_pnl += pnl;
        m_total_closes++;

        // 거래 기록 저장
        m_trades.push_back(tr);
        cap_trades();

        // SymbolLearner에 전달 → 스코어링
        m_learner.record_trade(tr);

        spdlog::info("[SHADOW] CLOSE {} {} {} entry={:.6f} exit={:.6f} pnl={:.4f} reason={} (total: {}/{})",
            vp.symbol, vp.timeframe, vp.side,
            vp.entry_price, exit_price, pnl, reason,
            m_wins, m_total_closes);

        m_positions.erase(it);
        maybe_save();
    }

    // ── 거래 기록 제한 ──
    void cap_trades() {
        constexpr size_t MAX_TRADES = 5000;
        if (m_trades.size() > MAX_TRADES) {
            m_trades.erase(m_trades.begin(),
                           m_trades.begin() + static_cast<ptrdiff_t>(m_trades.size() - MAX_TRADES));
        }
    }

    // ── 주기적 저장 (10건마다) ──
    void maybe_save() {
        m_ops_since_save++;
        if (m_ops_since_save >= 10) {
            save_state_locked();
            m_ops_since_save = 0;
        }
    }

    // ── 영속화: 저장 ──
    void save_state_locked() {
        try {
            nlohmann::json j;

            // 가상 포지션 저장
            auto pos_arr = nlohmann::json::array();
            for (auto& [key, vp] : m_positions) {
                pos_arr.push_back(nlohmann::json{
                    {"key", key},
                    {"symbol", vp.symbol},
                    {"timeframe", vp.timeframe},
                    {"side", vp.side},
                    {"strategy", vp.strategy},
                    {"entry_price", vp.entry_price},
                    {"sl_price", vp.sl_price},
                    {"tp1_price", vp.tp1_price},
                    {"opened_at", std::chrono::system_clock::to_time_t(vp.opened_at)}
                });
            }
            j["positions"] = pos_arr;

            // 거래 기록 저장 (최근 1000건만)
            auto tr_arr = nlohmann::json::array();
            size_t start = m_trades.size() > 1000 ? m_trades.size() - 1000 : 0;
            for (size_t i = start; i < m_trades.size(); i++) {
                auto& t = m_trades[i];
                tr_arr.push_back(nlohmann::json{
                    {"symbol", t.symbol}, {"timeframe", t.timeframe},
                    {"entry_price", t.entry_price}, {"exit_price", t.exit_price},
                    {"quantity", t.quantity}, {"pnl", t.pnl}, {"fee", t.fee},
                    {"exit_reason", t.exit_reason}, {"strategy", t.strategy}
                });
            }
            j["trades"] = tr_arr;

            // 통계 저장
            j["stats"] = nlohmann::json{
                {"total_entries", m_total_entries},
                {"total_closes", m_total_closes},
                {"wins", m_wins},
                {"losses", m_losses},
                {"total_pnl", m_total_pnl}
            };

            // Atomic write
            std::string path = m_data_dir + "/shadow_state.json";
            std::string tmp = path + ".tmp";
            std::ofstream ofs(tmp);
            ofs << j.dump(2);
            ofs.close();
            std::rename(tmp.c_str(), path.c_str());

        } catch (const std::exception& e) {
            spdlog::error("[SHADOW] Save error: {}", e.what());
        }
    }

    // ── 영속화: 로드 ──
    void load_state() {
        try {
            std::string path = m_data_dir + "/shadow_state.json";
            std::ifstream ifs(path);
            if (!ifs.good()) return;

            auto j = nlohmann::json::parse(ifs);

            // 포지션 복구
            if (j.contains("positions") && j["positions"].is_array()) {
                for (auto& p : j["positions"]) {
                    VirtualPosition vp;
                    vp.symbol      = p.value("symbol", "");
                    vp.timeframe   = p.value("timeframe", "0");
                    vp.side        = p.value("side", "long");
                    vp.strategy    = p.value("strategy", "unknown");
                    vp.entry_price = p.value("entry_price", 0.0);
                    vp.sl_price    = p.value("sl_price", 0.0);
                    vp.tp1_price   = p.value("tp1_price", 0.0);
                    auto t = p.value("opened_at", static_cast<int64_t>(0));
                    vp.opened_at = std::chrono::system_clock::from_time_t(t);

                    std::string key = p.value("key", make_key(vp.symbol, vp.timeframe, vp.side));
                    if (!vp.symbol.empty()) {
                        m_positions[key] = vp;
                    }
                }
            }

            // 거래 기록 복구
            if (j.contains("trades") && j["trades"].is_array()) {
                for (auto& t : j["trades"]) {
                    TradeRecord tr;
                    tr.symbol      = t.value("symbol", "");
                    tr.timeframe   = t.value("timeframe", "");
                    tr.entry_price = t.value("entry_price", 0.0);
                    tr.exit_price  = t.value("exit_price", 0.0);
                    tr.quantity    = t.value("quantity", 0.0);
                    tr.pnl         = t.value("pnl", 0.0);
                    tr.fee         = t.value("fee", 0.0);
                    tr.exit_reason = t.value("exit_reason", "");
                    tr.strategy    = t.value("strategy", "unknown");
                    m_trades.push_back(tr);
                }
            }

            // 통계 복구
            if (j.contains("stats")) {
                auto& s = j["stats"];
                m_total_entries = s.value("total_entries", uint64_t{0});
                m_total_closes  = s.value("total_closes", uint64_t{0});
                m_wins          = s.value("wins", 0);
                m_losses        = s.value("losses", 0);
                m_total_pnl     = s.value("total_pnl", 0.0);
            }

            spdlog::info("[SHADOW] Loaded: {} positions, {} trades, {}/{} W/L, pnl={:.2f}",
                m_positions.size(), m_trades.size(), m_wins, m_losses, m_total_pnl);

        } catch (const std::exception& e) {
            spdlog::warn("[SHADOW] Load error (starting fresh): {}", e.what());
        }
    }

    // ── 멤버 ──
    mutable std::mutex m_mtx;
    std::unordered_map<std::string, VirtualPosition> m_positions;
    std::vector<TradeRecord> m_trades;
    SymbolLearner& m_learner;
    std::string m_data_dir;

    // 통계
    uint64_t m_total_entries{0};
    uint64_t m_total_closes{0};
    int m_wins{0};
    int m_losses{0};
    double m_total_pnl{0.0};
    int m_ops_since_save{0};
};

} // namespace hft

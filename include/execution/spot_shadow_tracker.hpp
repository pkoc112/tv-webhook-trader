// ============================================================================
// execution/spot_shadow_tracker.hpp -- Spot Shadow Learning: 현물 시그널 가상 추적
// 2026-03-17 | 실전 매매와 완전 분리된 현물 전용 학습 트래커
//
// 목적:
//   Upbit/Bitget 현물 시그널을 무조건 가상 진입/청산하여
//   심볼×TF×거래소별 실적을 체점. 이 데이터를 기반으로 실전 매매 시
//   "이 심볼은 점수가 좋으니 진입해도 괜찮아" vs "무시해" 결정.
//
// 설계:
//   - process_signal() 최상단에서 호출 (어떤 필터보다 먼저)
//   - 자체 가상 포지션 맵 (실전과 완전 분리)
//   - 무제한 포지션 (한도 없음)
//   - 고정 노셔널: Bitget spot = 100 USDT, Upbit spot = 50000 KRW
//   - 현물은 LONG ONLY (buy 진입, sell = TP/SL 청산)
//   - 자체 내부 스코어링 (SymbolLearner에 record_trade 하지 않음)
//   - 부분 청산 지원: TP1=33%, TP2=50%, TP3=100%
//   - 수수료: Upbit 0.05% RT, Bitget spot 0.1% RT
//   - 영속화: data/spot_shadow_state.json
// ============================================================================
#pragma once

#include <mutex>
#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <fstream>
#include <algorithm>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include "webhook/signal_types.hpp"
#include "risk/symbol_scorer.hpp"  // TradeRecord

namespace hft {

// Forward declaration (참조만 유지, record_trade 호출하지 않음)
class SymbolLearner;

class SpotShadowTracker {
public:
    explicit SpotShadowTracker(SymbolLearner& learner,
                               const std::string& data_dir = "data")
        : m_learner(learner)
        , m_data_dir(data_dir)
    {
        load_state();
        spdlog::info("[SPOT_SHADOW] Initialized: {} virtual positions, {} completed trades",
            m_positions.size(), m_total_closes);
    }

    // ================================================================
    // 메인 입력점: 모든 시그널을 여기로 보낸다 (필터 이전에 호출)
    // 현물 시그널만 처리 (선물은 별도 ShadowTracker 관리)
    // ================================================================
    void track(const WebhookSignal& sig) {
        if (sig.market_type != "spot") return;   // 현물만 처리
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

    struct SpotShadowStats {
        uint64_t total_entries{0};
        uint64_t total_closes{0};
        uint64_t open_positions{0};
        int wins{0};
        int losses{0};
        double total_pnl{0.0};
        double win_rate{0.0};
    };

    [[nodiscard]] SpotShadowStats get_stats() const {
        std::lock_guard lock(m_mtx);
        SpotShadowStats s;
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
                {"exchange", vp.exchange},
                {"strategy", vp.strategy},
                {"entry_price", vp.entry_price},
                {"sl_price", vp.sl_price},
                {"tp1_price", vp.tp1_price},
                {"remaining_pct", vp.remaining_pct},
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
                {"exchange", t.exchange},
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
    // key = symbol:tf:exchange
    // ================================================================
    [[nodiscard]] nlohmann::json get_symbol_report() const {
        std::lock_guard lock(m_mtx);
        struct SymStats {
            int total{0}, wins{0};
            double total_pnl{0.0};
        };
        std::unordered_map<std::string, SymStats> by_symbol;

        for (auto& t : m_trades) {
            std::string key = t.symbol + ":" + t.timeframe + ":" + t.exchange;
            auto& s = by_symbol[key];
            s.total++;
            if (t.pnl > 0) s.wins++;
            s.total_pnl += t.pnl;
        }

        auto arr = nlohmann::json::array();
        for (auto& [sym, s] : by_symbol) {
            double wr = s.total > 0
                ? std::round(static_cast<double>(s.wins) / s.total * 10000.0) / 100.0
                : 0.0;
            std::string grade = compute_grade(s.total, s.wins, s.total_pnl, wr);

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
    // 단일 심볼 등급 조회 (grade filter에서 사용)
    // symbol = "BTC:5m:upbit" 형식
    // ================================================================
    [[nodiscard]] std::string get_grade(const std::string& symbol) const {
        std::lock_guard lock(m_mtx);
        auto it = m_symbol_grades.find(symbol);
        if (it != m_symbol_grades.end()) return it->second;
        return "?";
    }

    // ================================================================
    // 주기적 저장 (execution engine의 periodic_tasks에서 호출)
    // ================================================================
    void save_state() {
        std::lock_guard lock(m_mtx);
        save_state_locked();
    }

private:
    // ── 가상 포지션 구조체 (현물은 LONG ONLY) ──
    struct VirtualPosition {
        std::string symbol;
        std::string timeframe;
        std::string exchange;       // "upbit" / "bitget"
        std::string strategy;
        double entry_price{0};
        double sl_price{0};
        double tp1_price{0};
        double remaining_pct{100.0};  // 잔여 비율 (부분 청산용, %)
        std::chrono::system_clock::time_point opened_at;
    };

    // ── 키 포맷: symbol:tf:exchange ──
    static std::string make_key(const std::string& symbol,
                                 const std::string& tf,
                                 const std::string& exchange) {
        return symbol + ":" + tf + ":" + exchange;
    }

    // ── 거래소별 노셔널 ──
    static double get_notional(const std::string& exchange) {
        if (exchange == "upbit") return 50000.0;  // 50000 KRW
        return 100.0;  // 100 USDT (Bitget spot)
    }

    // ── 거래소별 수수료 (round-trip) ──
    static double get_fee_rate(const std::string& exchange) {
        if (exchange == "upbit") return 0.0005;   // 0.05% RT
        return 0.001;                              // 0.1% RT (Bitget spot)
    }

    // ── TP 부분 청산 비율 ──
    static double tp_close_pct(const std::string& tp_level) {
        if (tp_level == "TP1") return 33.0;
        if (tp_level == "TP2") return 50.0;
        return 100.0;  // TP3 or unknown → 전량 청산
    }

    // ── 등급 계산 ──
    static std::string compute_grade(int total, int wins, double total_pnl, double wr) {
        if (total < 5) return "?";           // 데이터 부족
        if (wr >= 65.0 && total_pnl > 0) return "A+";
        if (wr >= 55.0 && total_pnl > 0) return "A";
        if (wr >= 50.0 && total_pnl > 0) return "B";
        if (wr >= 45.0) return "C";
        if (wr >= 35.0) return "D";
        return "F";
    }

    // ── 내부 등급 캐시 갱신 ──
    void refresh_grade(const std::string& grade_key) {
        int total = 0, wins = 0;
        double total_pnl = 0.0;

        for (auto& t : m_trades) {
            std::string key = t.symbol + ":" + t.timeframe + ":" + t.exchange;
            if (key == grade_key) {
                total++;
                if (t.pnl > 0) wins++;
                total_pnl += t.pnl;
            }
        }

        double wr = total > 0
            ? std::round(static_cast<double>(wins) / total * 10000.0) / 100.0
            : 0.0;
        m_symbol_grades[grade_key] = compute_grade(total, wins, total_pnl, wr);
    }

    // ── Entry 처리: 현물은 buy만 (LONG ONLY) ──
    void track_entry(const WebhookSignal& sig) {
        // 현물은 LONG ONLY: buy만 진입
        if (sig.action != "buy") {
            spdlog::debug("[SPOT_SHADOW] Ignoring non-buy entry: {} {}", sig.symbol, sig.action);
            return;
        }

        std::string tf = sig.timeframe.empty() ? "0" : sig.timeframe;
        std::string key = make_key(sig.symbol, tf, sig.exchange);

        // 이미 같은 포지션이 있으면 스킵 (중복 방지)
        if (m_positions.count(key)) {
            spdlog::debug("[SPOT_SHADOW] Already have position: {}", key);
            return;
        }

        VirtualPosition vp;
        vp.symbol         = sig.symbol;
        vp.timeframe      = tf;
        vp.exchange       = sig.exchange;
        vp.strategy       = sig.strategy_name;
        vp.entry_price    = sig.price;
        vp.sl_price       = sig.sl;
        vp.tp1_price      = sig.tp1;
        vp.remaining_pct  = 100.0;
        vp.opened_at      = std::chrono::system_clock::now();

        m_positions[key] = vp;
        m_total_entries++;

        spdlog::info("[SPOT_SHADOW] ENTRY {} {} {} @ {:.6f} (total open: {})",
            vp.symbol, tf, vp.exchange, vp.entry_price, m_positions.size());

        maybe_save();
    }

    // ── TP 처리 (부분 청산 지원) ──
    void track_tp(const WebhookSignal& sig) {
        // TP에서 signal_direction="bull" → 원래 long 포지션 청산
        // 현물은 LONG ONLY이므로 bull 방향만 처리
        if (sig.signal_direction != "bull") {
            spdlog::debug("[SPOT_SHADOW] TP ignoring non-bull direction: {}", sig.signal_direction);
            return;
        }

        std::string tf = sig.timeframe.empty() ? "0" : sig.timeframe;
        std::string key = make_key(sig.symbol, tf, sig.exchange);

        auto it = m_positions.find(key);
        if (it == m_positions.end()) {
            // TF/거래소가 안 맞을 수 있으므로 같은 symbol로 검색
            it = find_by_symbol(sig.symbol);
            if (it == m_positions.end()) {
                spdlog::debug("[SPOT_SHADOW] TP no match: {}", key);
                return;
            }
        }

        // 부분 청산 비율 결정
        double close_pct = tp_close_pct(sig.tp_level);
        double actual_close_pct = std::min(close_pct, it->second.remaining_pct);

        if (actual_close_pct >= it->second.remaining_pct) {
            // 전량 청산
            close_position(it, sig.price, "TP_" + sig.tp_level, 100.0);
        } else {
            // 부분 청산
            partial_close(it, sig.price, "TP_" + sig.tp_level, actual_close_pct);
        }
    }

    // ── SL 처리 (전량 청산) ──
    void track_sl(const WebhookSignal& sig) {
        // SL에서 signal_direction="bull" → 원래 long 포지션 청산
        if (sig.signal_direction != "bull") {
            spdlog::debug("[SPOT_SHADOW] SL ignoring non-bull direction: {}", sig.signal_direction);
            return;
        }

        std::string tf = sig.timeframe.empty() ? "0" : sig.timeframe;
        std::string key = make_key(sig.symbol, tf, sig.exchange);

        auto it = m_positions.find(key);
        if (it == m_positions.end()) {
            it = find_by_symbol(sig.symbol);
            if (it == m_positions.end()) {
                spdlog::debug("[SPOT_SHADOW] SL no match: {}", key);
                return;
            }
        }

        close_position(it, sig.price, "SL", 100.0);
    }

    // ── 같은 symbol로 가상 포지션 검색 (TF/거래소 무관) ──
    using PosIter = std::unordered_map<std::string, VirtualPosition>::iterator;

    PosIter find_by_symbol(const std::string& symbol) {
        for (auto it = m_positions.begin(); it != m_positions.end(); ++it) {
            if (it->second.symbol == symbol) {
                return it;
            }
        }
        return m_positions.end();
    }

    // ── 부분 청산 (포지션 유지, PnL 기록) ──
    void partial_close(PosIter it, double exit_price,
                       const std::string& reason, double close_pct)
    {
        auto& vp = it->second;

        double notional = get_notional(vp.exchange);
        double fee_rate = get_fee_rate(vp.exchange);

        // 전체 포지션 기준 수량
        double total_qty = (vp.entry_price > 0) ? notional / vp.entry_price : 0;
        // 이번에 청산하는 비율 (잔여 대비가 아닌, 원래 포지션 대비)
        double close_ratio = (close_pct / 100.0) * (vp.remaining_pct / 100.0);
        double close_qty = total_qty * close_ratio;

        // 현물은 LONG ONLY: PnL = (exit - entry) * qty
        double pnl_raw = (exit_price - vp.entry_price) * close_qty;
        double fee = (vp.entry_price * close_qty + exit_price * close_qty) * fee_rate;
        double pnl = pnl_raw - fee;

        // TradeRecord 생성
        TradeRecord tr;
        tr.symbol      = vp.symbol;
        tr.timeframe   = vp.timeframe;
        tr.exit_reason = "SPOT_SHADOW_" + reason;
        tr.entry_price = vp.entry_price;
        tr.exit_price  = exit_price;
        tr.quantity    = close_qty;
        tr.pnl         = pnl;
        tr.fee         = fee;
        tr.strategy    = vp.strategy;
        tr.exchange    = vp.exchange;
        tr.market_type = "spot";

        // 통계 업데이트
        if (pnl > 0) m_wins++;
        else m_losses++;
        m_total_pnl += pnl;
        m_total_closes++;

        // 거래 기록 저장
        m_trades.push_back(tr);
        cap_trades();

        // 등급 캐시 갱신
        std::string grade_key = vp.symbol + ":" + vp.timeframe + ":" + vp.exchange;
        refresh_grade(grade_key);

        // 잔여 비율 업데이트
        double new_remaining = vp.remaining_pct * (1.0 - close_pct / 100.0);

        spdlog::info("[SPOT_SHADOW] PARTIAL_CLOSE {} {} {} {:.0f}% entry={:.6f} exit={:.6f} pnl={:.4f} reason={} remain={:.0f}%",
            vp.symbol, vp.timeframe, vp.exchange,
            close_pct, vp.entry_price, exit_price, pnl, reason, new_remaining);

        vp.remaining_pct = new_remaining;

        // 잔여가 거의 없으면 포지션 제거
        if (vp.remaining_pct < 1.0) {
            m_positions.erase(it);
        }

        maybe_save();
    }

    // ── 가상 포지션 전량 청산 + PnL 기록 ──
    void close_position(PosIter it, double exit_price,
                        const std::string& reason, double /*close_pct*/)
    {
        auto& vp = it->second;

        double notional = get_notional(vp.exchange);
        double fee_rate = get_fee_rate(vp.exchange);

        // 잔여 비율 기준 수량
        double total_qty = (vp.entry_price > 0) ? notional / vp.entry_price : 0;
        double close_qty = total_qty * (vp.remaining_pct / 100.0);

        // 현물은 LONG ONLY: PnL = (exit - entry) * qty
        double pnl_raw = (exit_price - vp.entry_price) * close_qty;
        double fee = (vp.entry_price * close_qty + exit_price * close_qty) * fee_rate;
        double pnl = pnl_raw - fee;

        // TradeRecord 생성
        TradeRecord tr;
        tr.symbol      = vp.symbol;
        tr.timeframe   = vp.timeframe;
        tr.exit_reason = "SPOT_SHADOW_" + reason;
        tr.entry_price = vp.entry_price;
        tr.exit_price  = exit_price;
        tr.quantity    = close_qty;
        tr.pnl         = pnl;
        tr.fee         = fee;
        tr.strategy    = vp.strategy;
        tr.exchange    = vp.exchange;
        tr.market_type = "spot";

        // 통계 업데이트
        if (pnl > 0) m_wins++;
        else m_losses++;
        m_total_pnl += pnl;
        m_total_closes++;

        // 거래 기록 저장
        m_trades.push_back(tr);
        cap_trades();

        // 등급 캐시 갱신
        std::string grade_key = vp.symbol + ":" + vp.timeframe + ":" + vp.exchange;
        refresh_grade(grade_key);

        spdlog::info("[SPOT_SHADOW] CLOSE {} {} {} entry={:.6f} exit={:.6f} pnl={:.4f} reason={} (total: {}/{})",
            vp.symbol, vp.timeframe, vp.exchange,
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
                    {"exchange", vp.exchange},
                    {"strategy", vp.strategy},
                    {"entry_price", vp.entry_price},
                    {"sl_price", vp.sl_price},
                    {"tp1_price", vp.tp1_price},
                    {"remaining_pct", vp.remaining_pct},
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
                    {"exchange", t.exchange},
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

            // 등급 캐시 저장
            j["grades"] = m_symbol_grades;

            // Atomic write: temp file + rename
            std::string path = m_data_dir + "/spot_shadow_state.json";
            std::string tmp = path + ".tmp";
            std::ofstream ofs(tmp);
            ofs << j.dump(2);
            ofs.close();
            std::rename(tmp.c_str(), path.c_str());

        } catch (const std::exception& e) {
            spdlog::error("[SPOT_SHADOW] Save error: {}", e.what());
        }
    }

    // ── 영속화: 로드 ──
    void load_state() {
        try {
            std::string path = m_data_dir + "/spot_shadow_state.json";
            std::ifstream ifs(path);
            if (!ifs.good()) return;

            auto j = nlohmann::json::parse(ifs);

            // 포지션 복구
            if (j.contains("positions") && j["positions"].is_array()) {
                for (auto& p : j["positions"]) {
                    VirtualPosition vp;
                    vp.symbol         = p.value("symbol", "");
                    vp.timeframe      = p.value("timeframe", "0");
                    vp.exchange       = p.value("exchange", "bitget");
                    vp.strategy       = p.value("strategy", "unknown");
                    vp.entry_price    = p.value("entry_price", 0.0);
                    vp.sl_price       = p.value("sl_price", 0.0);
                    vp.tp1_price      = p.value("tp1_price", 0.0);
                    vp.remaining_pct  = p.value("remaining_pct", 100.0);
                    auto t = p.value("opened_at", static_cast<int64_t>(0));
                    vp.opened_at = std::chrono::system_clock::from_time_t(t);

                    std::string key = p.value("key",
                        make_key(vp.symbol, vp.timeframe, vp.exchange));
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
                    tr.exchange    = t.value("exchange", "bitget");
                    tr.market_type = "spot";
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

            // 등급 캐시 복구
            if (j.contains("grades") && j["grades"].is_object()) {
                for (auto& [k, v] : j["grades"].items()) {
                    m_symbol_grades[k] = v.get<std::string>();
                }
            }

            spdlog::info("[SPOT_SHADOW] Loaded: {} positions, {} trades, {}/{} W/L, pnl={:.2f}, {} grades",
                m_positions.size(), m_trades.size(), m_wins, m_losses, m_total_pnl,
                m_symbol_grades.size());

        } catch (const std::exception& e) {
            spdlog::warn("[SPOT_SHADOW] Load error (starting fresh): {}", e.what());
        }
    }

    // ── 멤버 ──
    mutable std::mutex m_mtx;
    std::unordered_map<std::string, VirtualPosition> m_positions;
    std::vector<TradeRecord> m_trades;
    std::unordered_map<std::string, std::string> m_symbol_grades;  // 등급 캐시
    SymbolLearner& m_learner;  // 참조 유지 (record_trade 호출하지 않음)
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

// ============================================================================
// risk/symbol_learner.hpp -- 적응형 학습 엔진 v1.0
// 2026-03-15 | L1~L4 통합 학습 시스템
//
// L1: 심볼 성과 필터링 (승률/연패 기반 쿨다운 + 블랙리스트)
// L2: 동적 TP/SL 최적화 (심볼별 수익폭/손실폭 통계 → 최적 비율)
// L3: 시그널 품질 스코어링 (타임프레임별 + 시간대별 승률)
// L4: 포지션 사이징 최적화 (Kelly Criterion 변형)
//
// 단일 입력점: record_trade() → 모든 학습 업데이트
// 파일 영속화: data/learner_state.json (재시작 시 학습 유지)
// ============================================================================
#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <filesystem>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "risk/symbol_scorer.hpp"  // TradeRecord

namespace hft {

// -- 심볼별 학습 데이터 --
struct SymbolLearnData {
    // L1: 성과 필터링
    int    total_trades{0};
    int    wins{0};
    int    losses{0};
    int    consecutive_losses{0};
    int    max_consecutive_losses{0};
    double total_pnl{0.0};
    double ema_pnl{0.0};          // EMA 기반 최근 성과 (alpha=0.15)
    int64_t cooldown_until{0};    // epoch seconds, 0 = no cooldown
    bool   blacklisted{false};

    // L2: TP/SL 최적화
    double sum_win_pct{0.0};      // 승리 시 가격 변동 % 합계
    double sum_loss_pct{0.0};     // 손실 시 가격 변동 % 합계 (양수)
    int    win_count_pct{0};      // 유효한 win_pct 수
    int    loss_count_pct{0};     // 유효한 loss_pct 수
    double optimal_tp_pct{0.015}; // 학습된 최적 TP % (기본 1.5%)
    double optimal_sl_pct{0.010}; // 학습된 최적 SL % (기본 1.0%)

    // L3: 시그널 품질
    std::unordered_map<std::string, std::pair<int,int>> tf_stats;   // tf -> {wins, total}
    std::array<std::pair<int,int>, 24> hour_stats{};                // hour -> {wins, total}

    // L4: 포지션 사이징
    double avg_win_pct{0.0};
    double avg_loss_pct{0.0};
    double kelly_fraction{0.0};   // 계산된 Kelly 비율
};

// -- 학습 판단 결과 --
struct LearnDecision {
    bool   allowed{true};
    double signal_quality{0.5};   // 0.0~1.0
    double size_multiplier{1.0};  // Kelly 기반 사이즈 배율
    double tp_pct{0.015};         // 최적 TP %
    double sl_pct{0.010};         // 최적 SL %
    std::string skip_reason;
};

class SymbolLearner {
public:
    explicit SymbolLearner(const nlohmann::json& config = {},
                           const std::string& data_dir = "data")
        : m_data_dir(data_dir)
    {
        auto cfg = config.value("learner", nlohmann::json::object());
        m_enabled = cfg.value("enabled", true);

        // L1 config
        m_min_trades_filter   = cfg.value("min_trades_filter", 8);
        m_blacklist_min       = cfg.value("blacklist_min_trades", 10);
        m_blacklist_wr        = cfg.value("blacklist_win_rate", 0.25);
        m_cooldown_streak     = cfg.value("cooldown_consecutive_losses", 3);
        m_cooldown_minutes    = cfg.value("cooldown_minutes", 30);
        m_ema_alpha           = cfg.value("ema_alpha", 0.15);

        // L2 config
        m_min_trades_tpsl     = cfg.value("min_trades_tpsl", 15);
        m_default_tp_pct      = cfg.value("default_tp_pct", 0.015);
        m_default_sl_pct      = cfg.value("default_sl_pct", 0.010);
        m_tp_safety_margin    = cfg.value("tp_safety_margin", 0.85);  // 평균의 85%
        m_sl_safety_margin    = cfg.value("sl_safety_margin", 1.15);  // 평균의 115%

        // L3 config
        m_min_trades_quality  = cfg.value("min_trades_quality", 5);
        m_quality_threshold   = cfg.value("quality_skip_threshold", 0.20);

        // L4 config
        m_kelly_fraction_cap  = cfg.value("kelly_fraction_cap", 0.25);  // 최대 25%
        m_kelly_min_trades    = cfg.value("kelly_min_trades", 20);

        load_state();
        spdlog::info("[LEARNER] Initialized: enabled={} symbols_loaded={}", m_enabled, m_data.size());
    }

    // ================================================================
    // 단일 입력점: 거래 완료 시 호출
    // ================================================================
    void record_trade(const TradeRecord& tr) {
        if (!m_enabled || tr.symbol.empty()) return;

        std::lock_guard lock(m_mtx);
        auto& d = m_data[tr.symbol];

        // --- 기본 통계 업데이트 ---
        d.total_trades++;
        d.total_pnl += tr.pnl;

        bool is_win = tr.pnl > 0;
        if (is_win) {
            d.wins++;
            d.consecutive_losses = 0;
        } else {
            d.losses++;
            d.consecutive_losses++;
            d.max_consecutive_losses = std::max(d.max_consecutive_losses, d.consecutive_losses);
        }

        // EMA PnL (alpha=0.15, 최근 거래에 가중치)
        if (d.total_trades == 1) {
            d.ema_pnl = tr.pnl;
        } else {
            d.ema_pnl = m_ema_alpha * tr.pnl + (1.0 - m_ema_alpha) * d.ema_pnl;
        }

        // --- L2: 가격 변동폭 수집 ---
        if (tr.entry_price > 0 && tr.exit_price > 0) {
            double move_pct = std::abs(tr.exit_price - tr.entry_price) / tr.entry_price;
            if (is_win) {
                d.sum_win_pct += move_pct;
                d.win_count_pct++;
            } else {
                d.sum_loss_pct += move_pct;
                d.loss_count_pct++;
            }
            recalc_optimal_tpsl(d);
        }

        // --- L3: TF별 + 시간대별 통계 ---
        if (!tr.timeframe.empty()) {
            auto& [tw, tt] = d.tf_stats[tr.timeframe];
            tt++;
            if (is_win) tw++;
        }
        // 시간대 (현재 UTC hour)
        {
            auto now = std::chrono::system_clock::now();
            auto t = std::chrono::system_clock::to_time_t(now);
            std::tm tm{};
#ifdef _WIN32
            gmtime_s(&tm, &t);
#else
            gmtime_r(&t, &tm);
#endif
            int hour = tm.tm_hour;
            d.hour_stats[hour].second++;
            if (is_win) d.hour_stats[hour].first++;
        }

        // --- L4: Kelly 재계산 ---
        recalc_kelly(d);

        // --- L1: 쿨다운/블랙리스트 판정 ---
        update_filters(d);

        // 주기적 저장 (매 10 거래)
        m_trade_count_since_save++;
        if (m_trade_count_since_save >= 10) {
            save_state_locked();
            m_trade_count_since_save = 0;
        }

        spdlog::info("[LEARNER] {} trade#{} pnl={:.4f} wr={:.1f}% streak={} ema={:.4f} kelly={:.3f}",
            tr.symbol, d.total_trades, tr.pnl,
            d.total_trades > 0 ? (double)d.wins / d.total_trades * 100.0 : 0.0,
            d.consecutive_losses, d.ema_pnl, d.kelly_fraction);
    }

    // ================================================================
    // 통합 판단: 진입 시 호출
    // ================================================================
    [[nodiscard]] LearnDecision evaluate(const std::string& symbol,
                                          const std::string& timeframe,
                                          int current_hour_utc = -1) const {
        if (!m_enabled) {
            return LearnDecision{};  // 모든 기본값
        }

        std::lock_guard lock(m_mtx);
        LearnDecision dec;

        auto it = m_data.find(symbol);
        if (it == m_data.end()) {
            // 데이터 없음 → 기본값으로 허용
            dec.tp_pct = m_default_tp_pct;
            dec.sl_pct = m_default_sl_pct;
            return dec;
        }

        const auto& d = it->second;

        // [L1] 블랙리스트 체크
        if (d.blacklisted && d.total_trades >= m_blacklist_min) {
            dec.allowed = false;
            dec.skip_reason = "BLACKLISTED(wr=" +
                std::to_string(d.total_trades > 0 ? (double)d.wins/d.total_trades*100.0 : 0).substr(0,5) + "%)";
            return dec;
        }

        // [L1] 쿨다운 체크
        if (d.cooldown_until > 0) {
            auto now_epoch = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            if (now_epoch < d.cooldown_until) {
                dec.allowed = false;
                int remaining = static_cast<int>(d.cooldown_until - now_epoch);
                dec.skip_reason = "COOLDOWN(" + std::to_string(remaining) + "s remaining, streak=" +
                    std::to_string(d.consecutive_losses) + ")";
                return dec;
            }
        }

        // [L3] 시그널 품질 계산
        dec.signal_quality = calc_signal_quality(d, timeframe, current_hour_utc);
        if (d.total_trades >= m_min_trades_quality && dec.signal_quality < m_quality_threshold) {
            dec.allowed = false;
            dec.skip_reason = "LOW_QUALITY(q=" +
                std::to_string(dec.signal_quality).substr(0,5) + ")";
            return dec;
        }

        // [L2] 최적 TP/SL
        if (d.total_trades >= m_min_trades_tpsl) {
            dec.tp_pct = d.optimal_tp_pct;
            dec.sl_pct = d.optimal_sl_pct;
        } else {
            dec.tp_pct = m_default_tp_pct;
            dec.sl_pct = m_default_sl_pct;
        }

        // [L4] 포지션 사이즈 배율 (Kelly 기반)
        if (d.total_trades >= m_kelly_min_trades && d.kelly_fraction > 0) {
            // Kelly를 직접 사용하지 않고, 0.5~1.5 범위의 배율로 변환
            // kelly_fraction이 높으면 → 더 큰 포지션
            dec.size_multiplier = std::clamp(0.5 + d.kelly_fraction * 4.0, 0.5, 1.5);
        } else {
            dec.size_multiplier = 1.0;
        }

        // 품질 기반 추가 조정: 품질 낮으면 사이즈 축소
        if (d.total_trades >= m_min_trades_quality) {
            dec.size_multiplier *= std::clamp(dec.signal_quality * 1.5, 0.5, 1.2);
        }

        return dec;
    }

    // ================================================================
    // Dashboard API
    // ================================================================
    [[nodiscard]] nlohmann::json get_learner_json() const {
        std::lock_guard lock(m_mtx);
        auto arr = nlohmann::json::array();

        // 정렬: total_trades 기준 내림차순
        std::vector<std::pair<std::string, const SymbolLearnData*>> sorted;
        for (auto& [sym, d] : m_data) sorted.push_back({sym, &d});
        std::sort(sorted.begin(), sorted.end(),
            [](auto& a, auto& b) { return a.second->total_trades > b.second->total_trades; });

        for (auto& [sym, d] : sorted) {
            double wr = d->total_trades > 0 ? (double)d->wins / d->total_trades : 0.0;
            nlohmann::json entry;
            entry["symbol"] = sym;
            entry["total_trades"] = d->total_trades;
            entry["wins"] = d->wins;
            entry["losses"] = d->losses;
            entry["win_rate"] = std::round(wr * 1000.0) / 10.0;
            entry["total_pnl"] = std::round(d->total_pnl * 10000.0) / 10000.0;
            entry["ema_pnl"] = std::round(d->ema_pnl * 10000.0) / 10000.0;
            entry["consecutive_losses"] = d->consecutive_losses;
            entry["blacklisted"] = d->blacklisted;
            entry["cooldown_active"] = is_in_cooldown(*d);
            entry["optimal_tp_pct"] = std::round(d->optimal_tp_pct * 10000.0) / 100.0;  // as %
            entry["optimal_sl_pct"] = std::round(d->optimal_sl_pct * 10000.0) / 100.0;
            entry["kelly_fraction"] = std::round(d->kelly_fraction * 1000.0) / 1000.0;

            // Best timeframe
            std::string best_tf = "N/A";
            double best_wr = 0;
            for (auto& [tf, stats] : d->tf_stats) {
                if (stats.second >= 3) {
                    double tw = (double)stats.first / stats.second;
                    if (tw > best_wr) { best_wr = tw; best_tf = tf; }
                }
            }
            entry["best_tf"] = best_tf;
            entry["best_tf_wr"] = std::round(best_wr * 1000.0) / 10.0;

            arr.push_back(entry);
        }
        return arr;
    }

    [[nodiscard]] nlohmann::json get_summary_json() const {
        std::lock_guard lock(m_mtx);
        int total_symbols = static_cast<int>(m_data.size());
        int blacklisted = 0, cooled = 0, learned = 0;
        for (auto& [_, d] : m_data) {
            if (d.blacklisted) blacklisted++;
            if (is_in_cooldown(d)) cooled++;
            if (d.total_trades >= m_min_trades_tpsl) learned++;
        }
        return nlohmann::json{
            {"enabled", m_enabled},
            {"total_symbols", total_symbols},
            {"blacklisted", blacklisted},
            {"cooling_down", cooled},
            {"learned_tpsl", learned},
            {"min_trades_filter", m_min_trades_filter},
            {"min_trades_tpsl", m_min_trades_tpsl}
        };
    }

    void save() {
        std::lock_guard lock(m_mtx);
        save_state_locked();
    }

private:
    // ── L2: TP/SL 최적화 재계산 ──
    void recalc_optimal_tpsl(SymbolLearnData& d) {
        if (d.win_count_pct >= 5) {
            double avg_win = d.sum_win_pct / d.win_count_pct;
            // TP = 평균 승리폭 × safety_margin (보수적으로)
            d.optimal_tp_pct = std::clamp(avg_win * m_tp_safety_margin, 0.003, 0.05);
            d.avg_win_pct = avg_win;
        }
        if (d.loss_count_pct >= 5) {
            double avg_loss = d.sum_loss_pct / d.loss_count_pct;
            // SL = 평균 손실폭 × safety_margin (약간 넓게)
            d.optimal_sl_pct = std::clamp(avg_loss * m_sl_safety_margin, 0.002, 0.03);
            d.avg_loss_pct = avg_loss;
        }
    }

    // ── L3: 시그널 품질 계산 ──
    double calc_signal_quality(const SymbolLearnData& d,
                                const std::string& timeframe,
                                int hour_utc) const {
        double base_wr = d.total_trades > 0 ? (double)d.wins / d.total_trades : 0.5;

        // TF별 승률 가중치 (40%)
        double tf_quality = base_wr;
        if (!timeframe.empty()) {
            auto it = d.tf_stats.find(timeframe);
            if (it != d.tf_stats.end() && it->second.second >= m_min_trades_quality) {
                tf_quality = (double)it->second.first / it->second.second;
            }
        }

        // 시간대별 승률 가중치 (20%)
        double hour_quality = base_wr;
        if (hour_utc >= 0 && hour_utc < 24) {
            auto& [hw, ht] = d.hour_stats[hour_utc];
            if (ht >= 3) {
                hour_quality = (double)hw / ht;
            }
        }

        // EMA 트렌드 가중치 (20%) — 최근 수익 경향
        double trend_quality = 0.5;
        if (d.total_trades >= 5) {
            // ema_pnl 양수 → 좋음, 음수 → 나쁨
            trend_quality = std::clamp(0.5 + d.ema_pnl * 50.0, 0.0, 1.0);
        }

        // 연패 페널티 (20%)
        double streak_quality = 1.0;
        if (d.consecutive_losses >= 2) {
            streak_quality = std::max(0.1, 1.0 - d.consecutive_losses * 0.15);
        }

        // 복합 품질 = tf(40%) + hour(20%) + trend(20%) + streak(20%)
        double quality = tf_quality * 0.40 + hour_quality * 0.20 +
                         trend_quality * 0.20 + streak_quality * 0.20;
        return std::clamp(quality, 0.0, 1.0);
    }

    // ── L4: Kelly Criterion 재계산 ──
    void recalc_kelly(SymbolLearnData& d) {
        if (d.total_trades < m_kelly_min_trades) {
            d.kelly_fraction = 0.0;
            return;
        }
        double wr = (double)d.wins / d.total_trades;
        double lr = 1.0 - wr;

        // Kelly = W - L/R where R = avg_win / avg_loss
        if (d.avg_loss_pct > 0 && d.avg_win_pct > 0) {
            double R = d.avg_win_pct / d.avg_loss_pct;
            double kelly = wr - lr / R;
            // Half-Kelly for safety, capped
            d.kelly_fraction = std::clamp(kelly * 0.5, 0.0, m_kelly_fraction_cap);
        } else if (d.total_pnl > 0 && d.avg_loss_pct > 0) {
            // Fallback: simple profit-based fraction
            double avg_pnl = d.total_pnl / d.total_trades;
            d.kelly_fraction = std::clamp(avg_pnl * 10.0, 0.0, m_kelly_fraction_cap);
        } else {
            d.kelly_fraction = 0.0;
        }
    }

    // ── L1: 필터 업데이트 ──
    void update_filters(SymbolLearnData& d) {
        // 쿨다운: N연패 시 M분간 정지
        if (d.consecutive_losses >= m_cooldown_streak) {
            auto now_epoch = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            d.cooldown_until = now_epoch + m_cooldown_minutes * 60;
            spdlog::warn("[LEARNER] COOLDOWN: streak={} → {}min pause",
                d.consecutive_losses, m_cooldown_minutes);
        }

        // 블랙리스트: 충분한 거래 후 승률 25% 미만
        if (d.total_trades >= m_blacklist_min) {
            double wr = (double)d.wins / d.total_trades;
            if (wr < m_blacklist_wr) {
                if (!d.blacklisted) {
                    d.blacklisted = true;
                    spdlog::warn("[LEARNER] BLACKLISTED: trades={} wr={:.1f}%",
                        d.total_trades, wr * 100.0);
                }
            } else {
                // 승률 회복 시 블랙리스트 해제
                if (d.blacklisted && wr >= m_blacklist_wr + 0.05) {
                    d.blacklisted = false;
                    spdlog::info("[LEARNER] UN-BLACKLISTED: wr={:.1f}%", wr * 100.0);
                }
            }
        }
    }

    bool is_in_cooldown(const SymbolLearnData& d) const {
        if (d.cooldown_until <= 0) return false;
        auto now_epoch = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        return now_epoch < d.cooldown_until;
    }

    // ── Persistence ──
    void save_state_locked() {
        try {
            std::filesystem::create_directories(m_data_dir);
            nlohmann::json state;
            state["saved_at"] = iso_now();
            state["version"] = 1;

            auto& symbols = state["symbols"];
            for (auto& [sym, d] : m_data) {
                nlohmann::json s;
                s["total_trades"] = d.total_trades;
                s["wins"] = d.wins;
                s["losses"] = d.losses;
                s["consecutive_losses"] = d.consecutive_losses;
                s["max_consecutive_losses"] = d.max_consecutive_losses;
                s["total_pnl"] = d.total_pnl;
                s["ema_pnl"] = d.ema_pnl;
                s["cooldown_until"] = d.cooldown_until;
                s["blacklisted"] = d.blacklisted;
                s["sum_win_pct"] = d.sum_win_pct;
                s["sum_loss_pct"] = d.sum_loss_pct;
                s["win_count_pct"] = d.win_count_pct;
                s["loss_count_pct"] = d.loss_count_pct;
                s["optimal_tp_pct"] = d.optimal_tp_pct;
                s["optimal_sl_pct"] = d.optimal_sl_pct;
                s["avg_win_pct"] = d.avg_win_pct;
                s["avg_loss_pct"] = d.avg_loss_pct;
                s["kelly_fraction"] = d.kelly_fraction;

                // TF stats
                auto& tfs = s["tf_stats"];
                for (auto& [tf, st] : d.tf_stats) {
                    tfs[tf] = {{"w", st.first}, {"t", st.second}};
                }

                // Hour stats (only non-zero)
                auto& hrs = s["hour_stats"];
                for (int h = 0; h < 24; h++) {
                    if (d.hour_stats[h].second > 0) {
                        hrs[std::to_string(h)] = {{"w", d.hour_stats[h].first}, {"t", d.hour_stats[h].second}};
                    }
                }

                symbols[sym] = s;
            }

            // Atomic write
            auto path = m_data_dir + "/learner_state.json";
            auto tmp = path + ".tmp";
            {
                std::ofstream f(tmp);
                f << state.dump(2);
            }
            std::filesystem::rename(tmp, path);
        } catch (const std::exception& e) {
            spdlog::error("[LEARNER] Save failed: {}", e.what());
        }
    }

    void load_state() {
        auto path = m_data_dir + "/learner_state.json";
        if (!std::filesystem::exists(path)) return;
        try {
            std::ifstream f(path);
            auto state = nlohmann::json::parse(f);

            if (!state.contains("symbols")) return;

            std::lock_guard lock(m_mtx);
            for (auto& [sym, s] : state["symbols"].items()) {
                SymbolLearnData d;
                d.total_trades = s.value("total_trades", 0);
                d.wins = s.value("wins", 0);
                d.losses = s.value("losses", 0);
                d.consecutive_losses = s.value("consecutive_losses", 0);
                d.max_consecutive_losses = s.value("max_consecutive_losses", 0);
                d.total_pnl = s.value("total_pnl", 0.0);
                d.ema_pnl = s.value("ema_pnl", 0.0);
                d.cooldown_until = s.value("cooldown_until", (int64_t)0);
                d.blacklisted = s.value("blacklisted", false);
                d.sum_win_pct = s.value("sum_win_pct", 0.0);
                d.sum_loss_pct = s.value("sum_loss_pct", 0.0);
                d.win_count_pct = s.value("win_count_pct", 0);
                d.loss_count_pct = s.value("loss_count_pct", 0);
                d.optimal_tp_pct = s.value("optimal_tp_pct", 0.015);
                d.optimal_sl_pct = s.value("optimal_sl_pct", 0.010);
                d.avg_win_pct = s.value("avg_win_pct", 0.0);
                d.avg_loss_pct = s.value("avg_loss_pct", 0.0);
                d.kelly_fraction = s.value("kelly_fraction", 0.0);

                if (s.contains("tf_stats")) {
                    for (auto& [tf, st] : s["tf_stats"].items()) {
                        d.tf_stats[tf] = {st.value("w", 0), st.value("t", 0)};
                    }
                }
                if (s.contains("hour_stats")) {
                    for (auto& [h, st] : s["hour_stats"].items()) {
                        int hour = std::stoi(h);
                        if (hour >= 0 && hour < 24) {
                            d.hour_stats[hour] = {st.value("w", 0), st.value("t", 0)};
                        }
                    }
                }
                m_data[sym] = d;
            }
            spdlog::info("[LEARNER] Loaded {} symbols from state", m_data.size());
        } catch (const std::exception& e) {
            spdlog::error("[LEARNER] Load failed: {}", e.what());
        }
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

    // ── State ──
    mutable std::mutex m_mtx;
    std::unordered_map<std::string, SymbolLearnData> m_data;
    std::string m_data_dir;
    int m_trade_count_since_save{0};

    // Config
    bool   m_enabled{true};
    // L1
    int    m_min_trades_filter{8};
    int    m_blacklist_min{10};
    double m_blacklist_wr{0.25};
    int    m_cooldown_streak{3};
    int    m_cooldown_minutes{30};
    double m_ema_alpha{0.15};
    // L2
    int    m_min_trades_tpsl{15};
    double m_default_tp_pct{0.015};
    double m_default_sl_pct{0.010};
    double m_tp_safety_margin{0.85};
    double m_sl_safety_margin{1.15};
    // L3
    int    m_min_trades_quality{5};
    double m_quality_threshold{0.20};
    // L4
    double m_kelly_fraction_cap{0.25};
    int    m_kelly_min_trades{20};
};

} // namespace hft

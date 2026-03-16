// ============================================================================
// risk/symbol_learner.hpp -- 적응형 학습 엔진 v2.0
// 2026-03-15 | L1~L4 통합 학습 시스템
//
// v2.0 변경:
//   - 심볼+TF별 독립 추적 (key = "BTCUSDT:1", "BTCUSDT:5" 등)
//   - 1분봉 3연패 → 1분봉만 쿨다운, 5분봉은 정상 진입
//   - TF별 독립 TP/SL 최적화, Kelly 계산
//   - v1 learner_state.json 하위 호환 (symbol-only key 자동 변환)
//
// L1: 심볼+TF별 성과 필터링 (승률/연패 기반 쿨다운 + 블랙리스트)
// L2: 동적 TP/SL 최적화 (심볼+TF별 수익폭/손실폭 → 최적 비율)
// L3: 시그널 품질 스코어링 (시간대별 승률 + EMA 트렌드)
// L4: 포지션 사이징 최적화 (Kelly Criterion 변형)
//
// 단일 입력점: record_trade() → 모든 학습 업데이트
// 파일 영속화: data/learner_state.json (재시작 시 학습 유지)
// ============================================================================
#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
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

// -- 시장 세션 정의 --
enum class MarketSession { ASIA, EUROPE, US, OVERLAP_EU_US };

inline MarketSession get_session(int hour_utc) {
    // 아시아: 00-08 UTC (09-17 KST)
    if (hour_utc >= 0 && hour_utc < 8) return MarketSession::ASIA;
    // 유럽: 08-13 UTC
    if (hour_utc >= 8 && hour_utc < 13) return MarketSession::EUROPE;
    // EU+US 겹침: 13-17 UTC (변동성 최대)
    if (hour_utc >= 13 && hour_utc < 17) return MarketSession::OVERLAP_EU_US;
    // 미국: 17-24 UTC
    return MarketSession::US;
}

inline std::string session_name(MarketSession s) {
    switch (s) {
        case MarketSession::ASIA: return "ASIA";
        case MarketSession::EUROPE: return "EU";
        case MarketSession::OVERLAP_EU_US: return "EU_US";
        case MarketSession::US: return "US";
    }
    return "?";
}

// -- 심볼+TF별 학습 데이터 --
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

    // L3: 시그널 품질 (시간대별)
    std::array<std::pair<int,int>, 24> hour_stats{};  // hour -> {wins, total}

    // L3+: 세션별 성과 추적 (아시아/유럽/미국/겹침)
    std::array<std::pair<int,int>, 4> session_stats{};  // session -> {wins, total}

    // L4: 포지션 사이징
    double avg_win_pct{0.0};
    double avg_loss_pct{0.0};
    double kelly_fraction{0.0};   // 계산된 Kelly 비율

    // L5: 최근 N건 성과 (슬라이딩 윈도우)
    static constexpr int RECENT_WINDOW = 10;
    std::array<double, 10> recent_pnls{};  // 최근 10건 PnL 순환 버퍼
    int recent_idx{0};                      // 다음 쓸 위치
    int recent_count{0};                    // 채워진 수

    double recent_avg_pnl() const {
        if (recent_count == 0) return 0.0;
        double sum = 0;
        int n = std::min(recent_count, RECENT_WINDOW);
        for (int i = 0; i < n; i++) sum += recent_pnls[i];
        return sum / n;
    }

    double recent_win_rate() const {
        if (recent_count == 0) return 0.5;
        int n = std::min(recent_count, RECENT_WINDOW);
        int w = 0;
        for (int i = 0; i < n; i++) if (recent_pnls[i] > 0) w++;
        return (double)w / n;
    }
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
        m_tp_safety_margin    = cfg.value("tp_safety_margin", 0.85);
        m_sl_safety_margin    = cfg.value("sl_safety_margin", 1.15);

        // L3 config
        m_min_trades_quality  = cfg.value("min_trades_quality", 5);
        m_quality_threshold   = cfg.value("quality_skip_threshold", 0.20);

        // L4 config
        m_kelly_fraction_cap  = cfg.value("kelly_fraction_cap", 0.25);
        m_kelly_min_trades    = cfg.value("kelly_min_trades", 20);

        load_state();
        spdlog::info("[LEARNER] Initialized: enabled={} entries_loaded={}", m_enabled, m_data.size());
    }

    // ================================================================
    // 키 생성: "BTCUSDT:5" 형태
    // ================================================================
    static std::string make_key(const std::string& symbol, const std::string& tf) {
        if (tf.empty()) return symbol + ":all";
        return symbol + ":" + tf;
    }

    static std::pair<std::string, std::string> parse_key(const std::string& key) {
        auto pos = key.rfind(':');
        if (pos == std::string::npos) return {key, "all"};
        return {key.substr(0, pos), key.substr(pos + 1)};
    }

    // ================================================================
    // 단일 입력점: 거래 완료 시 호출
    // ================================================================
    void record_trade(const TradeRecord& tr) {
        if (!m_enabled || tr.symbol.empty()) return;

        std::lock_guard lock(m_mtx);
        std::string key = make_key(tr.symbol, tr.timeframe);
        auto& d = m_data[key];

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

        // --- L3: 시간대별 통계 + 세션별 추적 ---
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

            // 세션별 통계
            auto session = get_session(hour);
            int si = static_cast<int>(session);
            d.session_stats[si].second++;
            if (is_win) d.session_stats[si].first++;
        }

        // --- L5: 최근 N건 슬라이딩 윈도우 ---
        d.recent_pnls[d.recent_idx] = tr.pnl;
        d.recent_idx = (d.recent_idx + 1) % SymbolLearnData::RECENT_WINDOW;
        d.recent_count = std::min(d.recent_count + 1, SymbolLearnData::RECENT_WINDOW);

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

        spdlog::info("[LEARNER] {}:{} trade#{} pnl={:.4f} wr={:.1f}% streak={} ema={:.4f} kelly={:.3f}",
            tr.symbol, tr.timeframe.empty() ? "all" : tr.timeframe,
            d.total_trades, tr.pnl,
            d.total_trades > 0 ? (double)d.wins / d.total_trades * 100.0 : 0.0,
            d.consecutive_losses, d.ema_pnl, d.kelly_fraction);
    }

    // ================================================================
    // 통합 판단: 진입 시 호출 (symbol + timeframe 조합으로 조회)
    // ================================================================
    [[nodiscard]] LearnDecision evaluate(const std::string& symbol,
                                          const std::string& timeframe,
                                          int current_hour_utc = -1) const {
        if (!m_enabled) {
            return LearnDecision{};
        }

        std::lock_guard lock(m_mtx);
        LearnDecision dec;

        std::string key = make_key(symbol, timeframe);
        auto it = m_data.find(key);
        if (it == m_data.end()) {
            // 해당 symbol:tf 데이터 없음 → 기본값으로 허용
            dec.tp_pct = m_default_tp_pct;
            dec.sl_pct = m_default_sl_pct;
            return dec;
        }

        const auto& d = it->second;

        // [L1] 블랙리스트 체크
        if (d.blacklisted && d.total_trades >= m_blacklist_min) {
            dec.allowed = false;
            dec.skip_reason = "BLACKLISTED(" + key + " wr=" +
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
                dec.skip_reason = "COOLDOWN(" + key + " " + std::to_string(remaining) + "s, streak=" +
                    std::to_string(d.consecutive_losses) + ")";
                return dec;
            }
        }

        // [L3] 시그널 품질 계산
        dec.signal_quality = calc_signal_quality(d, current_hour_utc);
        if (d.total_trades >= m_min_trades_quality && dec.signal_quality < m_quality_threshold) {
            dec.allowed = false;
            dec.skip_reason = "LOW_QUALITY(" + key + " q=" +
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
            dec.size_multiplier = std::clamp(0.5 + d.kelly_fraction * 4.0, 0.5, 1.5);
        } else {
            dec.size_multiplier = 1.0;
        }

        // 품질 기반 추가 조정
        if (d.total_trades >= m_min_trades_quality) {
            dec.size_multiplier *= std::clamp(dec.signal_quality * 1.5, 0.5, 1.2);
        }

        // [L5] 연속 손실 시 점진적 사이즈 축소 (쿨다운 전에 선제적으로)
        if (d.consecutive_losses >= 2) {
            // 2연패: 70%, 3연패: 50%, 4연패+: 30%
            double streak_mult = std::max(0.3, 1.0 - d.consecutive_losses * 0.2);
            dec.size_multiplier *= streak_mult;
            spdlog::debug("[LEARNER] Streak penalty: streak={} mult={:.2f}",
                d.consecutive_losses, streak_mult);
        }

        // [L5] 최근 10건 성과 기반 추가 보정
        if (d.recent_count >= 5) {
            double rwr = d.recent_win_rate();
            if (rwr < 0.3) {
                // 최근 10건 중 승률 30% 미만 → 사이즈 50%
                dec.size_multiplier *= 0.5;
                spdlog::debug("[LEARNER] Recent WR penalty: rwr={:.1f}% mult=0.5", rwr * 100);
            } else if (rwr > 0.6) {
                // 최근 10건 중 승률 60% 초과 → 사이즈 120%
                dec.size_multiplier *= 1.2;
            }
        }

        // [L3+] 세션별 성과 필터
        if (current_hour_utc >= 0) {
            auto session = get_session(current_hour_utc);
            int si = static_cast<int>(session);
            auto& [sw, st] = d.session_stats[si];
            if (st >= 5) {  // 해당 세션에서 5건 이상 거래 시
                double session_wr = (double)sw / st;
                if (session_wr < 0.2) {
                    // 이 세션에서 승률 20% 미만 → 진입 거부
                    dec.allowed = false;
                    dec.skip_reason = "BAD_SESSION(" + session_name(session) +
                        " wr=" + std::to_string(session_wr * 100).substr(0,5) + "%)";
                    return dec;
                } else if (session_wr < 0.35) {
                    // 35% 미만 → 사이즈 축소
                    dec.size_multiplier *= 0.6;
                }
            }
        }

        // 최종 사이즈 배율 클램프
        dec.size_multiplier = std::clamp(dec.size_multiplier, 0.2, 2.0);

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
        for (auto& [key, d] : m_data) sorted.push_back({key, &d});
        std::sort(sorted.begin(), sorted.end(),
            [](auto& a, auto& b) { return a.second->total_trades > b.second->total_trades; });

        for (auto& [key, d] : sorted) {
            auto [sym, tf] = parse_key(key);
            double wr = d->total_trades > 0 ? (double)d->wins / d->total_trades : 0.0;
            nlohmann::json entry;
            entry["key"] = key;
            entry["symbol"] = sym;
            entry["timeframe"] = tf;
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

            // 세션별 성과
            nlohmann::json sess_json;
            const char* snames[] = {"ASIA", "EU", "EU_US", "US"};
            for (int i = 0; i < 4; i++) {
                auto& [sw, st] = d->session_stats[i];
                if (st > 0) {
                    sess_json[snames[i]] = {
                        {"wins", sw}, {"total", st},
                        {"win_rate", std::round((double)sw / st * 1000.0) / 10.0}
                    };
                }
            }
            entry["sessions"] = sess_json;

            // 최근 윈도우
            entry["recent_win_rate"] = std::round(d->recent_win_rate() * 1000.0) / 10.0;
            entry["recent_avg_pnl"] = std::round(d->recent_avg_pnl() * 10000.0) / 10000.0;

            arr.push_back(entry);
        }
        return arr;
    }

    [[nodiscard]] nlohmann::json get_summary_json() const {
        std::lock_guard lock(m_mtx);
        int total_entries = static_cast<int>(m_data.size());
        int blacklisted = 0, cooled = 0, learned = 0;

        // 고유 심볼 수 계산
        std::unordered_set<std::string> unique_symbols;
        for (auto& [key, d] : m_data) {
            auto [sym, tf] = parse_key(key);
            unique_symbols.insert(sym);
            if (d.blacklisted) blacklisted++;
            if (is_in_cooldown(d)) cooled++;
            if (d.total_trades >= m_min_trades_tpsl) learned++;
        }
        return nlohmann::json{
            {"enabled", m_enabled},
            {"total_entries", total_entries},
            {"total_symbols", static_cast<int>(unique_symbols.size())},
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
            d.optimal_tp_pct = std::clamp(avg_win * m_tp_safety_margin, 0.003, 0.05);
            d.avg_win_pct = avg_win;
        }
        if (d.loss_count_pct >= 5) {
            double avg_loss = d.sum_loss_pct / d.loss_count_pct;
            d.optimal_sl_pct = std::clamp(avg_loss * m_sl_safety_margin, 0.002, 0.03);
            d.avg_loss_pct = avg_loss;
        }
    }

    // ── L3: 시그널 품질 계산 (시간대+세션+최근윈도우 통합) ──
    double calc_signal_quality(const SymbolLearnData& d, int hour_utc) const {
        double base_wr = d.total_trades > 0 ? (double)d.wins / d.total_trades : 0.5;

        // 시간대별 승률 가중치 (20%)
        double hour_quality = base_wr;
        if (hour_utc >= 0 && hour_utc < 24) {
            auto& [hw, ht] = d.hour_stats[hour_utc];
            if (ht >= 3) {
                hour_quality = (double)hw / ht;
            }
        }

        // 세션별 승률 가중치 (15%) — 아시아/유럽/미국 세션
        double session_quality = base_wr;
        if (hour_utc >= 0) {
            auto session = get_session(hour_utc);
            int si = static_cast<int>(session);
            auto& [sw, st] = d.session_stats[si];
            if (st >= 3) {
                session_quality = (double)sw / st;
            }
        }

        // EMA 트렌드 가중치 (25%) — 전체 최근 수익 경향
        double trend_quality = 0.5;
        if (d.total_trades >= 5) {
            trend_quality = std::clamp(0.5 + d.ema_pnl * 50.0, 0.0, 1.0);
        }

        // 최근 윈도우 승률 가중치 (20%) — 최근 10건
        double recent_quality = 0.5;
        if (d.recent_count >= 3) {
            recent_quality = d.recent_win_rate();
        }

        // 연패 페널티 (20%)
        double streak_quality = 1.0;
        if (d.consecutive_losses >= 2) {
            streak_quality = std::max(0.1, 1.0 - d.consecutive_losses * 0.15);
        }

        // 복합 품질 = hour(20%) + session(15%) + trend(25%) + recent(20%) + streak(20%)
        double quality = hour_quality * 0.20
                       + session_quality * 0.15
                       + trend_quality * 0.25
                       + recent_quality * 0.20
                       + streak_quality * 0.20;
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

        if (d.avg_loss_pct > 0 && d.avg_win_pct > 0) {
            double R = d.avg_win_pct / d.avg_loss_pct;
            double kelly = wr - lr / R;
            d.kelly_fraction = std::clamp(kelly * 0.5, 0.0, m_kelly_fraction_cap);
        } else if (d.total_pnl > 0 && d.avg_loss_pct > 0) {
            double avg_pnl = d.total_pnl / d.total_trades;
            d.kelly_fraction = std::clamp(avg_pnl * 10.0, 0.0, m_kelly_fraction_cap);
        } else {
            d.kelly_fraction = 0.0;
        }
    }

    // ── L1: 필터 업데이트 (연패 기반 점진적 쿨다운) ──
    void update_filters(SymbolLearnData& d) {
        if (d.consecutive_losses >= m_cooldown_streak) {
            auto now_epoch = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            // 연패 수에 따라 쿨다운 시간 증가: 3연패=30분, 4연패=60분, 5연패=120분
            int extra_factor = std::min(d.consecutive_losses - m_cooldown_streak, 3);
            int cooldown_min = m_cooldown_minutes * (1 << extra_factor);  // 2^n 배
            d.cooldown_until = now_epoch + cooldown_min * 60;
            spdlog::warn("[LEARNER] COOLDOWN: streak={} → {}min pause (progressive)",
                d.consecutive_losses, cooldown_min);
        }

        if (d.total_trades >= m_blacklist_min) {
            double wr = (double)d.wins / d.total_trades;
            if (wr < m_blacklist_wr) {
                if (!d.blacklisted) {
                    d.blacklisted = true;
                    spdlog::warn("[LEARNER] BLACKLISTED: trades={} wr={:.1f}%",
                        d.total_trades, wr * 100.0);
                }
            } else {
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

    // ── Persistence (v2: symbol:tf keys) ──
    void save_state_locked() {
        try {
            std::filesystem::create_directories(m_data_dir);
            nlohmann::json state;
            state["saved_at"] = iso_now();
            state["version"] = 2;

            auto& entries = state["entries"];
            for (auto& [key, d] : m_data) {
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

                // Hour stats (only non-zero)
                auto& hrs = s["hour_stats"];
                for (int h = 0; h < 24; h++) {
                    if (d.hour_stats[h].second > 0) {
                        hrs[std::to_string(h)] = {{"w", d.hour_stats[h].first}, {"t", d.hour_stats[h].second}};
                    }
                }

                // Session stats
                auto& sess = s["session_stats"];
                const char* snames[] = {"ASIA", "EU", "EU_US", "US"};
                for (int i = 0; i < 4; i++) {
                    if (d.session_stats[i].second > 0) {
                        sess[snames[i]] = {{"w", d.session_stats[i].first}, {"t", d.session_stats[i].second}};
                    }
                }

                // Recent window
                s["recent_idx"] = d.recent_idx;
                s["recent_count"] = d.recent_count;
                auto& rpnls = s["recent_pnls"];
                for (int i = 0; i < std::min(d.recent_count, SymbolLearnData::RECENT_WINDOW); i++) {
                    rpnls.push_back(d.recent_pnls[i]);
                }

                entries[key] = s;
            }

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

    SymbolLearnData load_entry(const nlohmann::json& s) {
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

        if (s.contains("hour_stats")) {
            for (auto& [h, st] : s["hour_stats"].items()) {
                int hour = std::stoi(h);
                if (hour >= 0 && hour < 24) {
                    d.hour_stats[hour] = {st.value("w", 0), st.value("t", 0)};
                }
            }
        }

        // Session stats
        if (s.contains("session_stats")) {
            const std::pair<const char*, int> smap[] = {
                {"ASIA", 0}, {"EU", 1}, {"EU_US", 2}, {"US", 3}
            };
            for (auto& [name, idx] : smap) {
                if (s["session_stats"].contains(name)) {
                    auto& st = s["session_stats"][name];
                    d.session_stats[idx] = {st.value("w", 0), st.value("t", 0)};
                }
            }
        }

        // Recent window
        d.recent_idx = s.value("recent_idx", 0);
        d.recent_count = s.value("recent_count", 0);
        if (s.contains("recent_pnls") && s["recent_pnls"].is_array()) {
            int i = 0;
            for (auto& v : s["recent_pnls"]) {
                if (i >= SymbolLearnData::RECENT_WINDOW) break;
                d.recent_pnls[i++] = v.get<double>();
            }
        }

        return d;
    }

    void load_state() {
        auto path = m_data_dir + "/learner_state.json";
        if (!std::filesystem::exists(path)) return;
        try {
            std::ifstream f(path);
            auto state = nlohmann::json::parse(f);

            int version = state.value("version", 1);

            if (version >= 2 && state.contains("entries")) {
                // v2 format: key = "BTCUSDT:5"
                for (auto& [key, s] : state["entries"].items()) {
                    m_data[key] = load_entry(s);
                }
            } else if (state.contains("symbols")) {
                // v1 format: key = "BTCUSDT" (backward compat)
                // v1에서는 tf_stats가 있으므로 이를 활용해 TF별로 분할
                for (auto& [sym, s] : state["symbols"].items()) {
                    if (s.contains("tf_stats") && !s["tf_stats"].empty()) {
                        // TF별로 분할 (균등 배분)
                        auto tf_stats = s["tf_stats"];
                        int num_tfs = static_cast<int>(tf_stats.size());
                        for (auto& [tf, st] : tf_stats.items()) {
                            std::string key = sym + ":" + tf;
                            SymbolLearnData d = load_entry(s);
                            // TF별 통계 조정
                            int tf_total = st.value("t", 0);
                            int tf_wins = st.value("w", 0);
                            d.total_trades = tf_total;
                            d.wins = tf_wins;
                            d.losses = tf_total - tf_wins;
                            // PnL 균등 분할
                            if (num_tfs > 0) {
                                d.total_pnl = d.total_pnl / num_tfs;
                            }
                            // 연패/EMA는 초기화 (TF별 분리 불가)
                            d.consecutive_losses = 0;
                            d.ema_pnl = d.total_pnl / std::max(d.total_trades, 1);
                            m_data[key] = d;
                        }
                    } else {
                        // TF 정보 없음 → "symbol:all"
                        m_data[sym + ":all"] = load_entry(s);
                    }
                }
            }

            spdlog::info("[LEARNER] Loaded {} entries from state (v{})", m_data.size(), version);
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
    std::unordered_map<std::string, SymbolLearnData> m_data;  // key = "SYMBOL:TF"
    std::string m_data_dir;
    int m_trade_count_since_save{0};

    // Config
    bool   m_enabled{true};
    int    m_min_trades_filter{8};
    int    m_blacklist_min{10};
    double m_blacklist_wr{0.25};
    int    m_cooldown_streak{3};
    int    m_cooldown_minutes{30};
    double m_ema_alpha{0.15};
    int    m_min_trades_tpsl{15};
    double m_default_tp_pct{0.015};
    double m_default_sl_pct{0.010};
    double m_tp_safety_margin{0.85};
    double m_sl_safety_margin{1.15};
    int    m_min_trades_quality{5};
    double m_quality_threshold{0.20};
    double m_kelly_fraction_cap{0.25};
    int    m_kelly_min_trades{20};
};

} // namespace hft

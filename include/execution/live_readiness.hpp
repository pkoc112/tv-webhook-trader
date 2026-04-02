// ============================================================================
// execution/live_readiness.hpp -- Shadow → Live 전환 준비도 평가 엔진
// 2026-03-17 | 선물 ShadowTracker + 현물 SpotShadowTracker 데이터 통합 분석
//
// 목적:
//   Shadow/Paper 트레이딩 데이터를 기반으로 각 심볼의 실전 투입 준비도를
//   평가. BLOCKED → LEARNING → PROMISING → READY → PROVEN 파이프라인.
//
// 설계:
//   - ShadowTracker::get_symbol_report() 의 JSON 배열을 입력으로 받음
//   - 심볼별 grade, 거래 수, PnL을 조합하여 ReadinessLevel 결정
//   - 대시보드용 JSON 출력 (파이프라인 현황, 심볼별 상세)
//   - Header-only, thread-safe read (const methods)
// ============================================================================
#pragma once

#include <string>
#include <vector>
#include <algorithm>
#include <mutex>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace hft {

// ── Symbol readiness status ──
enum class ReadinessLevel {
    BLOCKED,     // D/F grade or negative trend - do NOT trade
    LEARNING,    // ? grade, insufficient data (<5 trades)
    PROMISING,   // C grade or B with few trades - keep watching
    READY,       // B+ grade, 10+ trades, positive PnL - can go live
    PROVEN       // A/A+ grade, 20+ trades - high confidence live
};

struct SymbolReadiness {
    std::string symbol;
    std::string market;       // "futures" or "spot"
    ReadinessLevel level;
    std::string grade;        // A+/A/B/C/D/F/?
    int total_trades{0};
    int wins{0};
    double win_rate{0.0};
    double total_pnl{0.0};
    double avg_pnl{0.0};
    int confidence_score{0};      // Consecutive passes (0-10)
    double confidence_boost{1.0}; // Position sizing multiplier
    std::string reason;           // Human-readable reason
};

// ── Confidence tracking entry (per symbol) ──
struct ConfidenceEntry {
    int consecutive_passes{0};   // How many consecutive rounds READY/PROVEN
    int confidence{0};           // Capped at max_confidence
    double size_boost{1.0};      // Position sizing multiplier

    // ★ Walk-forward demotion: rolling window of last 20 trades
    static constexpr int ROLLING_WINDOW = 20;
    double rolling_pnl{0.0};     // Sum of PnL for last N trades
    double rolling_wr{0.0};      // Win rate for last N trades
    int rolling_count{0};        // Number of trades in rolling window
    bool demoted{false};         // Was demoted in current cycle
};

struct PipelineStatus {
    int total_symbols{0};
    int blocked{0};
    int learning{0};
    int promising{0};
    int ready{0};
    int proven{0};
    bool can_go_live{false};  // At least N symbols are READY/PROVEN
    int min_symbols_for_live{5};
};

// ============================================================================
// LiveReadinessEngine
// ============================================================================
class LiveReadinessEngine {
public:
    // config can have:
    //   "live_readiness": {
    //     "min_trades_ready": 10,
    //     "min_trades_proven": 20,
    //     "min_grade_ready": "B",
    //     "min_grade_proven": "A",
    //     "min_symbols_for_live": 5,
    //     "require_positive_pnl": true
    //   }
    explicit LiveReadinessEngine(const nlohmann::json& config = {},
                                const std::string& data_dir = "data")
        : m_data_dir(data_dir)
    {
        auto cfg = config.value("live_readiness", nlohmann::json::object());
        m_min_trades_ready    = cfg.value("min_trades_ready", 10);
        m_min_trades_proven   = cfg.value("min_trades_proven", 20);
        m_min_grade_ready     = cfg.value("min_grade_ready", std::string("B"));
        m_min_grade_proven    = cfg.value("min_grade_proven", std::string("A"));
        m_min_symbols_for_live = cfg.value("min_symbols_for_live", 5);
        m_require_positive_pnl = cfg.value("require_positive_pnl", true);
        m_max_confidence      = cfg.value("max_confidence", 10);

        load_confidence();

        spdlog::info("[READINESS] Initialized: min_trades_ready={}, min_trades_proven={}, "
                     "min_grade_ready={}, min_grade_proven={}, min_symbols_for_live={}, "
                     "require_positive_pnl={}, max_confidence={}, loaded_confidence={}",
            m_min_trades_ready, m_min_trades_proven,
            m_min_grade_ready, m_min_grade_proven,
            m_min_symbols_for_live, m_require_positive_pnl,
            m_max_confidence, m_confidence.size());
    }

    // ================================================================
    // Evaluate a single symbol from shadow report data
    // ================================================================
    [[nodiscard]] SymbolReadiness evaluate_symbol(
        const nlohmann::json& entry, const std::string& market) const
    {
        SymbolReadiness r;
        r.symbol       = entry.value("symbol", "");
        r.market       = market;
        r.grade        = entry.value("grade", "?");
        r.total_trades = entry.value("total", 0);
        r.wins         = entry.value("wins", 0);
        r.win_rate     = entry.value("win_rate", 0.0);
        r.total_pnl    = entry.value("total_pnl", 0.0);
        r.avg_pnl      = entry.value("avg_pnl", 0.0);

        // ── Readiness decision tree ──
        const bool pnl_ok = (r.total_pnl > 0) || !m_require_positive_pnl;

        // ? grade or too few trades → LEARNING
        if (r.grade == "?" || r.total_trades < 5) {
            r.level = ReadinessLevel::LEARNING;
            r.reason = r.grade == "?"
                ? "Insufficient data (grade unknown)"
                : "Too few trades (" + std::to_string(r.total_trades) + " < 5)";
            return r;
        }

        // F or D → BLOCKED
        if (r.grade == "F" || r.grade == "D") {
            r.level = ReadinessLevel::BLOCKED;
            r.reason = "Poor grade (" + r.grade + ") - not suitable for live trading";
            return r;
        }

        // C grade
        if (r.grade == "C") {
            if (r.total_pnl > 0) {
                r.level = ReadinessLevel::PROMISING;
                r.reason = "Grade C with positive PnL - keep monitoring";
            } else {
                r.level = ReadinessLevel::BLOCKED;
                r.reason = "Grade C with negative PnL - blocked";
            }
            return r;
        }

        // ★ FIX: Tier cap for small samples — prevents premature promotion
        // Regardless of grade, small sample sizes cap the maximum readiness level
        // trades < 15 → max PROMISING (was: could reach READY with 5 trades)
        // trades < 25 → max PROMISING for B-grade (can't reach READY)
        const bool small_sample_cap_c = r.total_trades < 15;  // max tier = C equivalent
        const bool small_sample_cap_b = r.total_trades < 25;  // max tier = B equivalent

        // A+ or A grade
        if (r.grade == "A+" || r.grade == "A") {
            if (small_sample_cap_c) {
                // ★ FIX: Too few trades for any promotion, even with A grade
                r.level = ReadinessLevel::PROMISING;
                r.reason = "Grade " + r.grade + " but only " +
                    std::to_string(r.total_trades) + " trades (<15, small sample cap)";
            } else if (r.total_trades >= m_min_trades_proven && pnl_ok) {
                r.level = ReadinessLevel::PROVEN;
                r.reason = "Grade " + r.grade + " with " +
                    std::to_string(r.total_trades) + " trades - high confidence";
            } else if (r.total_trades >= m_min_trades_ready && pnl_ok) {
                // ★ FIX: READY requires min_trades_ready (30), not just positive PnL
                r.level = ReadinessLevel::READY;
                r.reason = "Grade " + r.grade + " with " +
                    std::to_string(r.total_trades) + " trades (need " +
                    std::to_string(m_min_trades_proven) + " for PROVEN)";
            } else if (pnl_ok) {
                r.level = ReadinessLevel::PROMISING;
                r.reason = "Grade " + r.grade + " with " +
                    std::to_string(r.total_trades) + " trades (need " +
                    std::to_string(m_min_trades_ready) + " for READY)";
            } else {
                r.level = ReadinessLevel::PROMISING;
                r.reason = "Grade " + r.grade + " but negative PnL - keep watching";
            }
            return r;
        }

        // B grade
        if (r.grade == "B") {
            if (small_sample_cap_c) {
                // ★ FIX: <15 trades with B grade → capped at PROMISING
                r.level = ReadinessLevel::PROMISING;
                r.reason = "Grade B but only " + std::to_string(r.total_trades) +
                    " trades (<15, small sample cap)";
            } else if (r.total_trades >= m_min_trades_ready && pnl_ok) {
                r.level = ReadinessLevel::READY;
                r.reason = "Grade B with " + std::to_string(r.total_trades) +
                    " trades and positive PnL - ready for live";
            } else if (r.total_trades < m_min_trades_ready) {
                r.level = ReadinessLevel::PROMISING;
                r.reason = "Grade B but only " + std::to_string(r.total_trades) +
                    " trades (need " + std::to_string(m_min_trades_ready) + ")";
            } else {
                // Enough trades but negative PnL
                r.level = ReadinessLevel::PROMISING;
                r.reason = "Grade B with enough trades but negative PnL";
            }
            return r;
        }

        // Fallback (should not reach here with standard grades)
        r.level = ReadinessLevel::LEARNING;
        r.reason = "Unrecognized grade: " + r.grade;
        return r;
    }

    // ================================================================
    // Evaluate all symbols from shadow reports (futures + spot)
    // ================================================================
    [[nodiscard]] std::vector<SymbolReadiness> evaluate_all(
        const nlohmann::json& futures_report,
        const nlohmann::json& spot_report) const
    {
        std::vector<SymbolReadiness> results;
        results.reserve(futures_report.size() + spot_report.size());

        for (auto& entry : futures_report) {
            results.push_back(evaluate_symbol(entry, "futures"));
        }
        for (auto& entry : spot_report) {
            results.push_back(evaluate_symbol(entry, "spot"));
        }

        // Sort by readiness level (PROVEN first), then by total_pnl descending
        std::sort(results.begin(), results.end(), [](const SymbolReadiness& a, const SymbolReadiness& b) {
            if (a.level != b.level)
                return static_cast<int>(a.level) > static_cast<int>(b.level);
            return a.total_pnl > b.total_pnl;
        });

        return results;
    }

    // ================================================================
    // Get pipeline status summary
    // ================================================================
    [[nodiscard]] PipelineStatus get_pipeline_status(
        const std::vector<SymbolReadiness>& readiness) const
    {
        PipelineStatus ps;
        ps.min_symbols_for_live = m_min_symbols_for_live;
        ps.total_symbols = static_cast<int>(readiness.size());

        for (auto& r : readiness) {
            switch (r.level) {
                case ReadinessLevel::BLOCKED:   ps.blocked++;   break;
                case ReadinessLevel::LEARNING:  ps.learning++;  break;
                case ReadinessLevel::PROMISING: ps.promising++; break;
                case ReadinessLevel::READY:     ps.ready++;     break;
                case ReadinessLevel::PROVEN:    ps.proven++;    break;
            }
        }

        ps.can_go_live = (ps.ready + ps.proven) >= m_min_symbols_for_live;
        return ps;
    }

    // ================================================================
    // Check if a specific symbol is allowed for live trading
    // ================================================================
    [[nodiscard]] bool is_live_allowed(
        const std::string& symbol, const std::string& market,
        const nlohmann::json& shadow_report) const
    {
        for (auto& entry : shadow_report) {
            if (entry.value("symbol", "") == symbol) {
                auto r = evaluate_symbol(entry, market);
                return r.level == ReadinessLevel::READY ||
                       r.level == ReadinessLevel::PROVEN;
            }
        }
        // Symbol not found in shadow report → not allowed
        return false;
    }

    // ================================================================
    // 심볼+TF별 평가 (같은 심볼이라도 TF별로 다른 등급 가능)
    // sym_tf_report: get_symbol_tf_report()의 결과 (symbol, timeframe, grade 포함)
    // ================================================================
    [[nodiscard]] std::vector<SymbolReadiness> evaluate_all_by_tf(
        const nlohmann::json& futures_tf_report,
        const nlohmann::json& spot_tf_report) const
    {
        std::vector<SymbolReadiness> results;
        results.reserve(futures_tf_report.size() + spot_tf_report.size());

        for (auto& entry : futures_tf_report) {
            auto r = evaluate_symbol(entry, "futures");
            // key를 symbol:tf 형식으로 교체 (대시보드 표시용)
            std::string tf = entry.value("timeframe", "");
            if (!tf.empty()) r.symbol = r.symbol + ":" + tf;
            results.push_back(std::move(r));
        }
        for (auto& entry : spot_tf_report) {
            auto r = evaluate_symbol(entry, "spot");
            std::string tf = entry.value("timeframe", "");
            if (!tf.empty()) r.symbol = r.symbol + ":" + tf;
            results.push_back(std::move(r));
        }

        std::sort(results.begin(), results.end(), [](const SymbolReadiness& a, const SymbolReadiness& b) {
            if (a.level != b.level)
                return static_cast<int>(a.level) > static_cast<int>(b.level);
            return a.total_pnl > b.total_pnl;
        });
        return results;
    }

    // ================================================================
    // 자동 전환용: 적격 심볼 Set 빌드 + 파이프라인 상태 반환
    // 주기적으로 호출 (60초마다). 결과를 캐시하여 주문 시 O(1) 조회.
    //
    // eligible_keys: "ZBCNUSDT" 형태의 READY/PROVEN 심볼 집합 (output)
    // 심볼 단위 합산 평가: 같은 심볼의 모든 TF 거래를 합산하여 등급 산정
    // Returns: PipelineStatus (can_go_live 포함)
    // ================================================================
    [[nodiscard]] PipelineStatus refresh_eligible(
        const nlohmann::json& futures_report,
        const nlohmann::json& spot_report,
        std::unordered_set<std::string>& eligible_keys)
    {
        eligible_keys.clear();
        auto all = evaluate_all(futures_report, spot_report);
        auto ps = get_pipeline_status(all);

        // Track confidence scores + walk-forward demotion
        {
            std::lock_guard lock(m_conf_mtx);
            std::unordered_set<std::string> seen;

            // ★ Build rolling stats from report data (last N trades)
            // The report entries contain total trades, wins, total_pnl
            // We use these as proxy for rolling performance
            auto build_rolling = [](const nlohmann::json& report,
                                    std::unordered_map<std::string, ConfidenceEntry>& conf) {
                for (auto& entry : report) {
                    std::string sym = entry.value("symbol", "");
                    if (sym.empty()) continue;
                    auto& ce = conf[sym];
                    // Use recent_trades/recent_wins/recent_pnl if available,
                    // otherwise fall back to total stats as approximation
                    int recent_n = entry.value("recent_trades", entry.value("total", 0));
                    int recent_wins = entry.value("recent_wins", entry.value("wins", 0));
                    double recent_pnl = entry.value("recent_pnl", entry.value("total_pnl", 0.0));
                    // Cap to rolling window size
                    ce.rolling_count = std::min(recent_n, ConfidenceEntry::ROLLING_WINDOW);
                    ce.rolling_pnl = recent_pnl;
                    ce.rolling_wr = ce.rolling_count > 0
                        ? static_cast<double>(std::min(recent_wins, ce.rolling_count)) / ce.rolling_count
                        : 0.0;
                }
            };
            build_rolling(futures_report, m_confidence);
            build_rolling(spot_report, m_confidence);

            for (auto& r : all) {
                seen.insert(r.symbol);
                auto& ce = m_confidence[r.symbol];
                ce.demoted = false;

                if (r.level == ReadinessLevel::READY || r.level == ReadinessLevel::PROVEN) {
                    // ★ Walk-forward demotion: check rolling window before accepting
                    bool should_demote = false;
                    std::string demote_reason;
                    if (ce.rolling_count >= ConfidenceEntry::ROLLING_WINDOW) {
                        if (ce.rolling_pnl < 0) {
                            should_demote = true;
                            demote_reason = "rolling_pnl < 0 over last " +
                                std::to_string(ConfidenceEntry::ROLLING_WINDOW) + " trades";
                        } else if (ce.rolling_wr < 0.40) {
                            should_demote = true;
                            demote_reason = "rolling_wr=" +
                                std::to_string(static_cast<int>(ce.rolling_wr * 100)) +
                                "% < 40% over last " +
                                std::to_string(ConfidenceEntry::ROLLING_WINDOW) + " trades";
                        }
                    }

                    if (should_demote) {
                        // Demote: READY/PROVEN → PROMISING, reset confidence
                        ce.consecutive_passes = 0;
                        ce.confidence = 0;
                        ce.size_boost = confidence_to_boost(0);
                        ce.demoted = true;
                        spdlog::warn("[READINESS] DEMOTE {} from {} to PROMISING: {}",
                            r.symbol, level_to_string(r.level), demote_reason);
                        // Do NOT add to eligible_keys (demoted)
                    } else {
                        ce.consecutive_passes = std::min(ce.consecutive_passes + 1, m_max_confidence);
                        eligible_keys.insert(r.symbol);
                    }
                } else {
                    if (ce.consecutive_passes > 0)
                        ce.consecutive_passes--;
                }
                ce.confidence = ce.consecutive_passes;
                ce.size_boost = confidence_to_boost(ce.confidence);
            }
            // Cleanup: remove entries that are 0 and not READY
            for (auto it = m_confidence.begin(); it != m_confidence.end(); ) {
                if (it->second.confidence == 0 && seen.find(it->first) != seen.end()
                    && eligible_keys.find(it->first) == eligible_keys.end()) {
                    it = m_confidence.erase(it);
                } else {
                    ++it;
                }
            }
            save_confidence();
        }

        if (ps.can_go_live) {
            // Count confidence distribution
            int c0=0, c1=0, c2=0, c3=0, c5=0;
            std::lock_guard lock(m_conf_mtx);
            for (auto& [sym, ce] : m_confidence) {
                if (ce.confidence == 0) c0++;
                else if (ce.confidence == 1) c1++;
                else if (ce.confidence <= 2) c2++;
                else if (ce.confidence <= 4) c3++;
                else c5++;
            }
            spdlog::info("[READINESS] 🟢 LIVE READY: {} READY + {} PROVEN = {} (need {}) | confidence: 0:{} 1:{} 2:{} 3-4:{} 5+:{}",
                ps.ready, ps.proven, ps.ready + ps.proven, ps.min_symbols_for_live,
                c0, c1, c2, c3, c5);
        }

        return ps;
    }

    // ── Thread-safe confidence accessor ──
    [[nodiscard]] ConfidenceEntry get_confidence(const std::string& symbol) const {
        std::lock_guard lock(m_conf_mtx);
        auto it = m_confidence.find(symbol);
        return it != m_confidence.end() ? it->second : ConfidenceEntry{};
    }

    // ================================================================
    // Get everything as JSON for dashboard
    // ================================================================
    [[nodiscard]] nlohmann::json get_readiness_json(
        const nlohmann::json& futures_report,
        const nlohmann::json& spot_report) const
    {
        auto all = evaluate_all(futures_report, spot_report);
        auto ps = get_pipeline_status(all);

        // Enrich with confidence data
        for (auto& r : all) {
            auto ce = get_confidence(r.symbol);
            r.confidence_score = ce.confidence;
            r.confidence_boost = ce.size_boost;
        }

        // Separate by market
        auto futures_arr = nlohmann::json::array();
        auto spot_arr = nlohmann::json::array();

        for (auto& r : all) {
            auto j = readiness_to_json(r);
            if (r.market == "futures")
                futures_arr.push_back(std::move(j));
            else
                spot_arr.push_back(std::move(j));
        }

        // Top 5 ready/proven symbols
        auto top5 = nlohmann::json::array();
        int count = 0;
        for (auto& r : all) {
            if (count >= 5) break;
            if (r.level == ReadinessLevel::READY || r.level == ReadinessLevel::PROVEN) {
                top5.push_back(nlohmann::json{
                    {"symbol", r.symbol},
                    {"market", r.market},
                    {"level", level_to_string(r.level)},
                    {"grade", r.grade},
                    {"total_trades", r.total_trades},
                    {"total_pnl", round4(r.total_pnl)},
                    {"confidence_score", r.confidence_score},
                    {"confidence_boost", round2(r.confidence_boost)}
                });
                count++;
            }
        }

        // Summary
        auto summary = nlohmann::json{
            {"total_symbols", ps.total_symbols},
            {"blocked", ps.blocked},
            {"learning", ps.learning},
            {"promising", ps.promising},
            {"ready", ps.ready},
            {"proven", ps.proven},
            {"can_go_live", ps.can_go_live},
            {"min_symbols_for_live", ps.min_symbols_for_live},
            {"top_ready", top5}
        };

        return nlohmann::json{
            {"pipeline", pipeline_to_json(ps)},
            {"futures", futures_arr},
            {"spot", spot_arr},
            {"summary", summary}
        };
    }

private:
    // ── Grade comparison helper: returns numeric value (higher = better) ──
    static inline int grade_to_rank(const std::string& grade) {
        if (grade == "A+") return 7;
        if (grade == "A")  return 6;
        if (grade == "B")  return 5;
        if (grade == "C")  return 4;
        if (grade == "D")  return 3;
        if (grade == "F")  return 2;
        if (grade == "?")  return 1;
        return 0;
    }

    // ── Convert ReadinessLevel to string ──
    static inline std::string level_to_string(ReadinessLevel level) {
        switch (level) {
            case ReadinessLevel::BLOCKED:   return "BLOCKED";
            case ReadinessLevel::LEARNING:  return "LEARNING";
            case ReadinessLevel::PROMISING: return "PROMISING";
            case ReadinessLevel::READY:     return "READY";
            case ReadinessLevel::PROVEN:    return "PROVEN";
        }
        return "UNKNOWN";
    }

    // ── SymbolReadiness → JSON ──
    static inline nlohmann::json readiness_to_json(const SymbolReadiness& r) {
        return nlohmann::json{
            {"symbol", r.symbol},
            {"market", r.market},
            {"level", level_to_string(r.level)},
            {"grade", r.grade},
            {"total_trades", r.total_trades},
            {"wins", r.wins},
            {"win_rate", round2(r.win_rate)},
            {"total_pnl", round4(r.total_pnl)},
            {"avg_pnl", round4(r.avg_pnl)},
            {"confidence_score", r.confidence_score},
            {"confidence_boost", round2(r.confidence_boost)},
            {"reason", r.reason}
        };
    }

    // ── PipelineStatus → JSON ──
    static inline nlohmann::json pipeline_to_json(const PipelineStatus& ps) {
        return nlohmann::json{
            {"total_symbols", ps.total_symbols},
            {"blocked", ps.blocked},
            {"learning", ps.learning},
            {"promising", ps.promising},
            {"ready", ps.ready},
            {"proven", ps.proven},
            {"can_go_live", ps.can_go_live},
            {"min_symbols_for_live", ps.min_symbols_for_live}
        };
    }

    // ── Rounding helpers ──
    static inline double round2(double v) { return std::round(v * 100.0) / 100.0; }
    static inline double round4(double v) { return std::round(v * 10000.0) / 10000.0; }

    // ── Confidence boost lookup ──
    double confidence_to_boost(int conf) const {
        if (conf <= 0) return 0.9;
        if (conf == 1) return 1.0;
        if (conf == 2) return 1.05;
        if (conf <= 4) return 1.1;
        return 1.15;  // 5+ (conservative, was 2.0x)
    }

    // ── Confidence persistence ──
    void save_confidence() {
        try {
            nlohmann::json j;
            for (auto& [sym, ce] : m_confidence) {
                j[sym] = nlohmann::json{
                    {"consecutive_passes", ce.consecutive_passes},
                    {"confidence", ce.confidence},
                    {"size_boost", ce.size_boost},
                    {"rolling_pnl", ce.rolling_pnl},
                    {"rolling_wr", ce.rolling_wr},
                    {"rolling_count", ce.rolling_count}
                };
            }
            std::string path = m_data_dir + "/confidence_scores.json";
            std::string tmp = path + ".tmp";
            std::ofstream ofs(tmp);
            ofs << j.dump(2);
            ofs.close();
            std::rename(tmp.c_str(), path.c_str());
        } catch (const std::exception& e) {
            spdlog::error("[READINESS] Confidence save error: {}", e.what());
        }
    }

    void load_confidence() {
        try {
            std::string path = m_data_dir + "/confidence_scores.json";
            std::ifstream ifs(path);
            if (!ifs.good()) return;
            auto j = nlohmann::json::parse(ifs);
            for (auto& [sym, val] : j.items()) {
                ConfidenceEntry ce;
                ce.consecutive_passes = val.value("consecutive_passes", 0);
                ce.confidence = val.value("confidence", 0);
                ce.size_boost = val.value("size_boost", 1.0);
                ce.rolling_pnl = val.value("rolling_pnl", 0.0);
                ce.rolling_wr = val.value("rolling_wr", 0.0);
                ce.rolling_count = val.value("rolling_count", 0);
                m_confidence[sym] = ce;
            }
        } catch (const std::exception& e) {
            spdlog::warn("[READINESS] Confidence load failed (starting fresh): {}", e.what());
        }
    }

    // ── Config ──
    int m_min_trades_ready{10};
    int m_min_trades_proven{20};
    std::string m_min_grade_ready{"B"};
    std::string m_min_grade_proven{"A"};
    int m_min_symbols_for_live{5};
    bool m_require_positive_pnl{true};
    int m_max_confidence{10};
    std::string m_data_dir{"data"};

    // ── Confidence state ──
    std::unordered_map<std::string, ConfidenceEntry> m_confidence;
    mutable std::mutex m_conf_mtx;
};

} // namespace hft

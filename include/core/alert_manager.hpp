// ============================================================================
// core/alert_manager.hpp -- 알림 관리자
// v1.0 | 2026-03-15 | 실시간 이벤트 추적 + 대시보드 API
//
// 순환 버퍼로 최근 500개 알림 저장
// 레벨: INFO, WARN, ERROR, CRITICAL
// ============================================================================
#pragma once

#include <string>
#include <vector>
#include <mutex>
#include <chrono>
#include <deque>
#include <nlohmann/json.hpp>

// Windows defines ERROR as a macro — undefine to avoid conflict with AlertLevel::ERROR
#ifdef ERROR
#undef ERROR
#endif

namespace hft {

enum class AlertLevel { INFO, WARN, ERROR, CRITICAL };

inline std::string alert_level_str(AlertLevel lv) {
    switch (lv) {
        case AlertLevel::INFO:     return "INFO";
        case AlertLevel::WARN:     return "WARN";
        case AlertLevel::ERROR:    return "ERROR";
        case AlertLevel::CRITICAL: return "CRITICAL";
    }
    return "UNKNOWN";
}

struct Alert {
    std::string timestamp;
    AlertLevel level;
    std::string category;   // "TRADE", "RISK", "SYSTEM", "CONNECTION"
    std::string message;
    std::string symbol;     // optional
};

class AlertManager {
public:
    static constexpr size_t MAX_ALERTS = 500;

    void add(AlertLevel level, const std::string& category,
             const std::string& message, const std::string& symbol = "")
    {
        Alert a;
        a.timestamp = iso_now();
        a.level = level;
        a.category = category;
        a.message = message;
        a.symbol = symbol;

        std::lock_guard lock(m_mtx);
        m_alerts.push_back(std::move(a));
        if (m_alerts.size() > MAX_ALERTS) {
            m_alerts.pop_front();
        }
    }

    // Convenience methods
    void info(const std::string& cat, const std::string& msg, const std::string& sym = "") {
        add(AlertLevel::INFO, cat, msg, sym);
    }
    void warn(const std::string& cat, const std::string& msg, const std::string& sym = "") {
        add(AlertLevel::WARN, cat, msg, sym);
    }
    void error(const std::string& cat, const std::string& msg, const std::string& sym = "") {
        add(AlertLevel::ERROR, cat, msg, sym);
    }
    void critical(const std::string& cat, const std::string& msg, const std::string& sym = "") {
        add(AlertLevel::CRITICAL, cat, msg, sym);
    }

    [[nodiscard]] nlohmann::json get_alerts_json(int limit = 100) const {
        std::lock_guard lock(m_mtx);
        auto arr = nlohmann::json::array();
        int start = static_cast<int>(m_alerts.size()) - limit;
        if (start < 0) start = 0;
        for (size_t i = static_cast<size_t>(start); i < m_alerts.size(); ++i) {
            auto& a = m_alerts[i];
            arr.push_back(nlohmann::json{
                {"timestamp", a.timestamp},
                {"level", alert_level_str(a.level)},
                {"category", a.category},
                {"message", a.message},
                {"symbol", a.symbol}
            });
        }
        return arr;
    }

    [[nodiscard]] size_t count() const {
        std::lock_guard lock(m_mtx);
        return m_alerts.size();
    }

private:
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

    mutable std::mutex m_mtx;
    std::deque<Alert> m_alerts;
};

} // namespace hft

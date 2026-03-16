// ============================================================================
// core/rate_limiter.hpp -- Token Bucket Rate Limiter
// v1.0 | 2026-03-13
// Bitget API 제한 준수: 10 req/s/UID (place-order), 10 req/s/UID (tpsl)
// 초과 시 429 응답 + 5분 차단이므로 반드시 준수해야 함
// ============================================================================
#pragma once

#include <mutex>
#include <chrono>
#include <algorithm>
#include <thread>

namespace hft {

class RateLimiter {
public:
    // rate: 초당 허용 요청 수 (예: 9.0 = 마진 두고 9개/초)
    explicit RateLimiter(double rate, double burst = 0)
        : m_rate(rate)
        , m_max_tokens(burst > 0 ? burst : rate)
        , m_tokens(burst > 0 ? burst : rate)
        , m_last_refill(std::chrono::steady_clock::now()) {}

    // 비차단: 토큰 있으면 소비하고 true, 없으면 false
    bool try_acquire(int n = 1) {
        std::lock_guard lock(m_mtx);
        refill();
        if (m_tokens >= static_cast<double>(n)) {
            m_tokens -= n;
            return true;
        }
        return false;
    }

    // 차단: 토큰이 생길 때까지 대기
    void acquire(int n = 1) {
        while (true) {
            {
                std::lock_guard lock(m_mtx);
                refill();
                if (m_tokens >= static_cast<double>(n)) {
                    m_tokens -= n;
                    return;
                }
            }
            // 다음 토큰 생성까지 대기
            auto wait_ms = static_cast<int>(1000.0 / m_rate) + 1;
            std::this_thread::sleep_for(std::chrono::milliseconds(wait_ms));
        }
    }

    // 현재 가용 토큰 수 (snapshot without refill).
    // Returns a stale-but-safe lower bound. We intentionally do NOT call
    // refill() here so that available() can remain a true const observer
    // with no side-effects. Callers who need an up-to-date count should
    // use try_acquire() instead.
    [[nodiscard]] double available() const {
        std::lock_guard lock(m_mtx);
        return m_tokens;
    }

private:
    void refill() {
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration<double>(now - m_last_refill).count();
        m_tokens = std::min(m_max_tokens, m_tokens + elapsed * m_rate);
        m_last_refill = now;
    }

    double m_rate;
    double m_max_tokens;
    double m_tokens;
    std::chrono::steady_clock::time_point m_last_refill;
    mutable std::mutex m_mtx;
};

// 심볼별 동시 작업 방지 (같은 심볼 중복 주문 방지)
class SymbolLockManager {
public:
    bool try_lock(const std::string& symbol) {
        std::lock_guard lock(m_mtx);
        return m_locked.insert(symbol).second;
    }

    void unlock(const std::string& symbol) {
        {
            std::lock_guard lock(m_mtx);
            m_locked.erase(symbol);
        }
        m_cv.notify_all();
    }

    // 심볼 잠금 대기 (최대 timeout)
    bool wait_lock(const std::string& symbol, std::chrono::milliseconds timeout) {
        std::unique_lock lock(m_mtx);
        if (m_locked.find(symbol) == m_locked.end()) {
            m_locked.insert(symbol);
            return true;
        }
        bool ok = m_cv.wait_for(lock, timeout, [&] {
            return m_locked.find(symbol) == m_locked.end();
        });
        if (ok) m_locked.insert(symbol);
        return ok;
    }

private:
    std::mutex m_mtx;
    std::condition_variable m_cv;
    std::unordered_set<std::string> m_locked;
};

// RAII 심볼 락 가드
class SymbolLockGuard {
public:
    SymbolLockGuard(SymbolLockManager& mgr, const std::string& symbol)
        : m_mgr(mgr), m_symbol(symbol) {}
    ~SymbolLockGuard() { m_mgr.unlock(m_symbol); }
    SymbolLockGuard(const SymbolLockGuard&) = delete;
    SymbolLockGuard& operator=(const SymbolLockGuard&) = delete;
private:
    SymbolLockManager& m_mgr;
    std::string m_symbol;
};

} // namespace hft

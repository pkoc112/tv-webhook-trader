// ============================================================================
// core/spsc_queue.hpp -- Thread-safe MPSC Signal Queue
// v2.0 | 2026-03-13 | SPSC -> MPSC 전환 (다중 webhook 스레드 안전)
//
// 변경 이력:
//   v1.0  초기 생성 (lock-free SPSC)
//   v2.0  mutex + condition_variable 기반 MPSC로 전환
//         - 다중 producer (webhook 스레드) 안전
//         - move semantics 지원
//         - condition_variable로 consumer 효율적 대기
//         - shutdown 지원
// ============================================================================
#pragma once

#include <mutex>
#include <condition_variable>
#include <deque>
#include <optional>
#include <chrono>
#include <cstddef>

namespace hft {

template <typename T>
class SPSCQueue {
public:
    explicit SPSCQueue(size_t max_capacity) : m_max_cap(max_capacity) {}

    SPSCQueue(const SPSCQueue&) = delete;
    SPSCQueue& operator=(const SPSCQueue&) = delete;

    // Thread-safe push (multiple producers OK)
    bool try_push(T&& item) {
        {
            std::lock_guard lock(m_mtx);
            if (m_queue.size() >= m_max_cap) return false;
            m_queue.push_back(std::move(item));
        }
        m_cv.notify_one();
        return true;
    }

    bool try_push(const T& item) {
        {
            std::lock_guard lock(m_mtx);
            if (m_queue.size() >= m_max_cap) return false;
            m_queue.push_back(item);
        }
        m_cv.notify_one();
        return true;
    }

    // Non-blocking pop
    [[nodiscard]] std::optional<T> try_pop() {
        std::lock_guard lock(m_mtx);
        if (m_queue.empty()) return std::nullopt;
        T item = std::move(m_queue.front());
        m_queue.pop_front();
        return item;
    }

    // Blocking pop with timeout (replaces busy-wait spin loop)
    [[nodiscard]] std::optional<T> wait_pop(std::chrono::milliseconds timeout) {
        std::unique_lock lock(m_mtx);
        if (!m_cv.wait_for(lock, timeout, [this] { return !m_queue.empty() || m_shutdown; }))
            return std::nullopt;
        if (m_queue.empty()) return std::nullopt;
        T item = std::move(m_queue.front());
        m_queue.pop_front();
        return item;
    }

    // Shutdown: wake up blocked consumer
    void shutdown() {
        {
            std::lock_guard lock(m_mtx);
            m_shutdown = true;
        }
        m_cv.notify_all();
    }

    [[nodiscard]] bool empty() const {
        std::lock_guard lock(m_mtx);
        return m_queue.empty();
    }

    [[nodiscard]] size_t size() const {
        std::lock_guard lock(m_mtx);
        return m_queue.size();
    }

private:
    mutable std::mutex m_mtx;
    std::condition_variable m_cv;
    std::deque<T> m_queue;
    size_t m_max_cap;
    bool m_shutdown{false};
};

} // namespace hft

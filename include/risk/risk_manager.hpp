// ============================================================================
// risk/risk_manager.hpp -- 리스크 관리
// v2.0 | 2026-03-13 | 포지션 추적, 심볼별 검증, 일일 리셋 타이머
//
// 변경 이력:
//   v1.0  초기 생성
//   v2.0  - 심볼별 포지션 추적 추가
//         - max_position_per_symbol 실제 검증
//         - update_pnl 연동 (서킷브레이커 활성화)
//         - 심볼 화이트리스트 검증
//         - 일일 리셋 타이머
// ============================================================================
#pragma once

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <chrono>
#include <spdlog/spdlog.h>
#include "core/types.hpp"

namespace hft {

struct RiskLimits {
    double max_position_per_symbol{1.0};
    double max_daily_loss{500.0};
    double max_order_size{0.5};
    int    max_open_orders{10};
    double circuit_breaker_loss{1000.0};
    double max_drawdown_pct{5.0};
};

class RiskManager {
public:
    explicit RiskManager(RiskLimits limits) : m_limits(limits) {}

    // 심볼 화이트리스트 설정
    void set_allowed_symbols(const std::unordered_set<std::string>& symbols) {
        std::lock_guard lock(m_mtx);
        m_allowed_symbols = symbols;
        spdlog::info("[Risk] Allowed symbols: {}", symbols.size());
    }

    // 주문 전 검증
    [[nodiscard]] bool validate(const OrderRequest& req) {
        if (m_circuit_breaker.load(std::memory_order_relaxed)) {
            spdlog::warn("[Risk] CIRCUIT BREAKER ACTIVE -- order rejected");
            return false;
        }

        if (req.quantity <= 0.0) {
            spdlog::warn("[Risk] Invalid order size: {}", req.quantity);
            return false;
        }

        if (req.quantity > m_limits.max_order_size) {
            spdlog::warn("[Risk] Order size {} exceeds max {}", req.quantity, m_limits.max_order_size);
            return false;
        }

        if (m_open_orders.load(std::memory_order_relaxed) >= m_limits.max_open_orders) {
            spdlog::warn("[Risk] Max open orders ({}) reached", m_limits.max_open_orders);
            return false;
        }

        std::lock_guard lock(m_mtx);

        // 심볼 화이트리스트 검증
        auto sym_str = req.symbol.str();
        if (!m_allowed_symbols.empty() &&
            m_allowed_symbols.find(sym_str) == m_allowed_symbols.end()) {
            spdlog::warn("[Risk] Symbol {} not in whitelist", sym_str);
            return false;
        }

        // 심볼별 포지션 한도
        auto it = m_positions.find(sym_str);
        if (it != m_positions.end()) {
            if (req.trade_side == TradeSide::Open && it->second.count >= m_limits.max_position_per_symbol) {
                spdlog::warn("[Risk] Max position for {} reached ({})",
                    sym_str, it->second.count);
                return false;
            }
        }

        // 일일 손실 한도
        if (m_daily_loss > m_limits.max_daily_loss) {
            spdlog::warn("[Risk] Daily loss limit reached: {:.2f}", m_daily_loss);
            return false;
        }

        return true;
    }

    void on_order_placed() { m_open_orders.fetch_add(1, std::memory_order_relaxed); }
    void on_order_done()   { m_open_orders.fetch_sub(1, std::memory_order_relaxed); }

    // 포지션 오픈 기록
    void on_position_opened(const std::string& symbol, double qty, const std::string& side) {
        std::lock_guard lock(m_mtx);
        auto& pos = m_positions[symbol];
        pos.count++;
        pos.total_qty += qty;
        pos.side = side;
        spdlog::info("[Risk] Position opened: {} {} qty={:.6f} (total={})",
            symbol, side, qty, pos.count);
    }

    // 포지션 클로즈 기록
    void on_position_closed(const std::string& symbol, double realized_pnl) {
        std::lock_guard lock(m_mtx);
        auto it = m_positions.find(symbol);
        if (it != m_positions.end()) {
            it->second.count--;
            if (it->second.count <= 0) {
                m_positions.erase(it);
            }
        }
        update_pnl_locked(realized_pnl);
    }

    // PnL 업데이트 (이미 lock 보유 시)
    void update_pnl(double realized_pnl) {
        std::lock_guard lock(m_mtx);
        update_pnl_locked(realized_pnl);
    }

    // 일일 리셋 (자정에 호출)
    void reset_daily() {
        std::lock_guard lock(m_mtx);
        spdlog::info("[Risk] Daily reset: PnL={:.2f} Loss={:.2f}", m_daily_pnl, m_daily_loss);
        m_daily_pnl = 0;
        m_daily_loss = 0;
        m_circuit_breaker.store(false, std::memory_order_relaxed);
        m_last_reset = std::chrono::steady_clock::now();
    }

    // 리셋 필요 여부 확인 (24시간 경과)
    bool needs_daily_reset() const {
        auto elapsed = std::chrono::steady_clock::now() - m_last_reset;
        return elapsed >= std::chrono::hours(24);
    }

    [[nodiscard]] double daily_pnl() const { std::lock_guard lock(m_mtx); return m_daily_pnl; }
    [[nodiscard]] double daily_loss() const { std::lock_guard lock(m_mtx); return m_daily_loss; }
    [[nodiscard]] bool is_circuit_breaker() const { return m_circuit_breaker.load(std::memory_order_relaxed); }
    [[nodiscard]] int open_positions_count() const {
        std::lock_guard lock(m_mtx);
        int total = 0;
        for (auto& [_, pos] : m_positions) total += pos.count;
        return total;
    }

private:
    struct PositionInfo {
        int count{0};
        double total_qty{0.0};
        std::string side;
    };

    void update_pnl_locked(double realized_pnl) {
        m_daily_pnl += realized_pnl;
        if (realized_pnl < 0) m_daily_loss -= realized_pnl;

        if (m_daily_loss >= m_limits.circuit_breaker_loss) {
            m_circuit_breaker.store(true, std::memory_order_relaxed);
            spdlog::error("[Risk] CIRCUIT BREAKER ACTIVATED -- daily loss: {:.2f}", m_daily_loss);
        }
    }

    RiskLimits m_limits;
    mutable std::mutex m_mtx;
    double m_daily_pnl{0};
    double m_daily_loss{0};
    std::atomic<int> m_open_orders{0};
    std::atomic<bool> m_circuit_breaker{false};
    std::unordered_map<std::string, PositionInfo> m_positions;
    std::unordered_set<std::string> m_allowed_symbols;
    std::chrono::steady_clock::time_point m_last_reset{std::chrono::steady_clock::now()};
};

} // namespace hft

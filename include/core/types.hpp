// ============================================================================
// core/types.hpp — 핵심 타입 정의
// v1.0 | 2026-03-13 | 초기 생성
// ============================================================================
#pragma once
#include <cstdint>
#include <array>
#include <chrono>
#include <algorithm>
#include <string>
#include <string_view>

namespace hft {

inline constexpr size_t CACHE_LINE_SIZE = 64;
using Timestamp = int64_t;
using Price     = double;
using Quantity  = double;
using OrderId   = uint64_t;

[[nodiscard]] inline Timestamp now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

[[nodiscard]] inline int64_t now_ms() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

struct Symbol {
    std::array<char, 24> data{};
    uint8_t length{0};
    Symbol() = default;
    explicit Symbol(std::string_view sv) noexcept {
        length = static_cast<uint8_t>(std::min(sv.size(), size_t{23}));
        std::copy_n(sv.data(), length, data.data());
    }
    [[nodiscard]] std::string_view view() const noexcept { return {data.data(), length}; }
    [[nodiscard]] std::string str() const { return std::string(data.data(), length); }
};

enum class Side : uint8_t       { Buy = 0, Sell = 1 };
enum class TradeSide : uint8_t  { Open = 0, Close = 1 };
enum class OrderType : uint8_t  { Market = 0, Limit = 1, PostOnly = 2 };
enum class TimeInForce : uint8_t { GTC = 0, IOC = 1, FOK = 2 };
enum class OrderStatus : uint8_t { New=0, PartiallyFilled=1, Filled=2, Cancelled=3, Rejected=4 };

struct alignas(CACHE_LINE_SIZE) OrderRequest {
    Symbol symbol;
    Side side;
    TradeSide trade_side;
    OrderType order_type;
    TimeInForce tif;
    Price price;
    Quantity quantity;
    OrderId client_order_id;
    Timestamp created_at;
    int32_t leverage{10};
    Price take_profit{0.0};
    Price stop_loss{0.0};
    std::array<char, 16> product_type{};
    std::array<char, 16> margin_coin{};
    std::array<char, 64> comment{};
};

struct alignas(CACHE_LINE_SIZE) OrderResponse {
    OrderId client_order_id;
    OrderId exchange_order_id;
    OrderStatus status;
    Price filled_price;
    Quantity filled_qty;
    Timestamp updated_at;
    int32_t error_code;
    std::array<char, 128> error_msg{};
};

} // namespace hft

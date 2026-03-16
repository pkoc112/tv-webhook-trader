// ============================================================================
// tests/test_risk_manager.cpp -- RiskManager::validate() unit tests
// ============================================================================
#include <gtest/gtest.h>
#include "risk/risk_manager.hpp"

using namespace hft;

// Helper: create an OrderRequest for testing
static OrderRequest make_order(const std::string& symbol_str = "BTCUSDT",
                               Side side = Side::Buy,
                               TradeSide trade_side = TradeSide::Open,
                               double qty = 0.1,
                               double price = 50000.0)
{
    OrderRequest req{};
    req.symbol = Symbol(symbol_str);
    req.side = side;
    req.trade_side = trade_side;
    req.order_type = OrderType::Market;
    req.tif = TimeInForce::IOC;
    req.price = price;
    req.quantity = qty;
    req.client_order_id = 1;
    req.created_at = now_ns();
    return req;
}

static RiskLimits default_limits() {
    RiskLimits lim;
    lim.max_position_per_symbol = 1.0;
    lim.max_daily_loss = 500.0;
    lim.max_order_size = 10.0;
    lim.max_open_orders = 5;
    lim.circuit_breaker_loss = 1000.0;
    lim.max_drawdown_pct = 5.0;
    return lim;
}

// ── Normal validation pass ──
TEST(RiskManager, NormalValidationPass) {
    RiskManager rm(default_limits());
    auto order = make_order("BTCUSDT", Side::Buy, TradeSide::Open, 0.1);

    EXPECT_TRUE(rm.validate(order));
}

// ── Max positions exceeded ──
TEST(RiskManager, MaxPositionsPerSymbolExceeded) {
    RiskManager rm(default_limits());  // max_position_per_symbol = 1

    // Open a position first
    rm.on_position_opened("BTCUSDT", 0.1, "long");

    // Try to open another for the same symbol
    auto order = make_order("BTCUSDT", Side::Buy, TradeSide::Open, 0.1);
    EXPECT_FALSE(rm.validate(order));
}

TEST(RiskManager, DifferentSymbolAllowed) {
    RiskManager rm(default_limits());

    rm.on_position_opened("BTCUSDT", 0.1, "long");

    // Different symbol should be allowed
    auto order = make_order("ETHUSDT", Side::Buy, TradeSide::Open, 0.1);
    EXPECT_TRUE(rm.validate(order));
}

TEST(RiskManager, CloseOrderBypassesPositionLimit) {
    RiskManager rm(default_limits());

    rm.on_position_opened("BTCUSDT", 0.1, "long");

    // Close orders should still pass (trade_side = Close skips position limit check)
    auto order = make_order("BTCUSDT", Side::Sell, TradeSide::Close, 0.1);
    EXPECT_TRUE(rm.validate(order));
}

// ── Daily loss limit exceeded ──
TEST(RiskManager, DailyLossLimitExceeded) {
    RiskManager rm(default_limits());  // max_daily_loss = 500

    // Accumulate losses
    rm.update_pnl(-300.0);
    rm.update_pnl(-250.0);  // total daily_loss = 550

    auto order = make_order();
    EXPECT_FALSE(rm.validate(order));
}

TEST(RiskManager, DailyLossResetAllows) {
    RiskManager rm(default_limits());

    rm.update_pnl(-600.0);  // exceeds daily loss
    auto order = make_order();
    EXPECT_FALSE(rm.validate(order));

    rm.reset_daily();
    EXPECT_TRUE(rm.validate(order));
}

// ── Symbol whitelist ──
TEST(RiskManager, SymbolNotInWhitelist) {
    RiskManager rm(default_limits());
    rm.set_allowed_symbols({"BTCUSDT", "ETHUSDT"});

    auto order_ok = make_order("BTCUSDT");
    EXPECT_TRUE(rm.validate(order_ok));

    auto order_bad = make_order("SOLUSDT");
    EXPECT_FALSE(rm.validate(order_bad));
}

TEST(RiskManager, EmptyWhitelistAllowsAll) {
    RiskManager rm(default_limits());
    // No whitelist set -> all symbols allowed

    auto order = make_order("ANYUSDT");
    EXPECT_TRUE(rm.validate(order));
}

// ── Max order size exceeded ──
TEST(RiskManager, MaxOrderSizeExceeded) {
    RiskManager rm(default_limits());  // max_order_size = 10.0

    auto order = make_order("BTCUSDT", Side::Buy, TradeSide::Open, 15.0);
    EXPECT_FALSE(rm.validate(order));
}

TEST(RiskManager, ZeroQuantityRejected) {
    RiskManager rm(default_limits());

    auto order = make_order("BTCUSDT", Side::Buy, TradeSide::Open, 0.0);
    EXPECT_FALSE(rm.validate(order));
}

TEST(RiskManager, NegativeQuantityRejected) {
    RiskManager rm(default_limits());

    auto order = make_order("BTCUSDT", Side::Buy, TradeSide::Open, -1.0);
    EXPECT_FALSE(rm.validate(order));
}

// ── Max open orders exceeded ──
TEST(RiskManager, MaxOpenOrdersExceeded) {
    auto limits = default_limits();
    limits.max_open_orders = 2;
    RiskManager rm(limits);

    rm.on_order_placed();
    rm.on_order_placed();

    auto order = make_order();
    EXPECT_FALSE(rm.validate(order));

    // Completing an order frees a slot
    rm.on_order_done();
    EXPECT_TRUE(rm.validate(order));
}

// ── Circuit breaker ──
TEST(RiskManager, CircuitBreakerActivation) {
    auto limits = default_limits();
    limits.circuit_breaker_loss = 100.0;
    RiskManager rm(limits);

    EXPECT_FALSE(rm.is_circuit_breaker());

    rm.update_pnl(-120.0);  // triggers circuit breaker
    EXPECT_TRUE(rm.is_circuit_breaker());

    auto order = make_order();
    EXPECT_FALSE(rm.validate(order));
}

TEST(RiskManager, CircuitBreakerResetOnDaily) {
    auto limits = default_limits();
    limits.circuit_breaker_loss = 100.0;
    RiskManager rm(limits);

    rm.update_pnl(-120.0);
    EXPECT_TRUE(rm.is_circuit_breaker());

    rm.reset_daily();
    EXPECT_FALSE(rm.is_circuit_breaker());

    auto order = make_order();
    EXPECT_TRUE(rm.validate(order));
}

// ── PnL tracking ──
TEST(RiskManager, PnLTracking) {
    RiskManager rm(default_limits());

    rm.update_pnl(50.0);
    rm.update_pnl(-20.0);
    rm.update_pnl(30.0);

    EXPECT_DOUBLE_EQ(rm.daily_pnl(), 60.0);
    EXPECT_DOUBLE_EQ(rm.daily_loss(), 20.0);  // only negative PnL counts
}

// ── Position tracking ──
TEST(RiskManager, PositionCountTracking) {
    RiskManager rm(default_limits());

    EXPECT_EQ(rm.open_positions_count(), 0);

    rm.on_position_opened("BTCUSDT", 0.1, "long");
    rm.on_position_opened("ETHUSDT", 1.0, "short");
    EXPECT_EQ(rm.open_positions_count(), 2);

    rm.on_position_closed("BTCUSDT", 10.0);
    EXPECT_EQ(rm.open_positions_count(), 1);

    rm.on_position_closed("ETHUSDT", -5.0);
    EXPECT_EQ(rm.open_positions_count(), 0);
}

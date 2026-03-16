// ============================================================================
// tests/test_portfolio_risk.cpp -- PortfolioRiskManager::check_entry() tests
// ============================================================================
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "risk/portfolio_risk.hpp"

using namespace hft;
using json = nlohmann::json;

// Helper: default config
static json make_config(int max_total = 50, double corr_factor = 0.7,
                        double max_corr_risk_pct = 50.0,
                        double daily_loss_pct = 5.0,
                        int cooldown_min = 60)
{
    return json{
        {"portfolio_risk", {
            {"max_positions_total", max_total},
            {"max_positions_per_tf", {{"5", 3}, {"15", 5}, {"60", 4}}},
            {"tf_exposure_pct", {{"5", 20.0}, {"15", 30.0}, {"60", 40.0}}},
            {"correlation_factor", corr_factor},
            {"max_correlated_risk_pct", max_corr_risk_pct},
            {"circuit_breaker", {
                {"daily_loss_pct", daily_loss_pct},
                {"weekly_loss_pct", 10.0},
                {"cooldown_minutes", cooldown_min}
            }}
        }}
    };
}

using Positions = std::unordered_map<std::string, ManagedPosition>;

static ManagedPosition make_pos(const std::string& symbol,
                                const std::string& tf,
                                double entry, double qty, int lev = 10)
{
    ManagedPosition p;
    p.symbol = symbol;
    p.timeframe = tf;
    p.side = "long";
    p.entry_price = entry;
    p.quantity = qty;
    p.leverage = lev;
    p.opened_at = std::chrono::system_clock::now();
    return p;
}

// ── Normal check_entry pass ──
TEST(PortfolioRisk, NormalEntryAllowed) {
    auto config = make_config();
    PortfolioRiskManager prm(config);

    Positions positions;
    auto decision = prm.check_entry(
        "BTCUSDT", "15", 50000.0, 0.01, 10, 10000.0, positions);

    EXPECT_TRUE(decision.allowed);
    EXPECT_TRUE(decision.reason.empty());
}

// ── Circuit breaker activation ──
TEST(PortfolioRisk, CircuitBreakerActivation) {
    auto config = make_config(50, 0.7, 50.0, 5.0, 9999);  // long cooldown
    PortfolioRiskManager prm(config);
    prm.update_balance(1000.0);

    // Trigger daily loss > 5% of 1000 = 50
    prm.on_trade_closed(-60.0);

    Positions positions;
    auto decision = prm.check_entry(
        "BTCUSDT", "15", 50000.0, 0.01, 10, 1000.0, positions);

    EXPECT_FALSE(decision.allowed);
    EXPECT_EQ(decision.check_failed, "circuit_breaker");
}

// ── Circuit breaker release after cooldown ──
// Note: This is tricky to test without mocking time. We test with
// cooldown_minutes = 0 which should release immediately.
TEST(PortfolioRisk, CircuitBreakerRelease) {
    auto config = make_config(50, 0.7, 50.0, 5.0, 0);  // cooldown = 0 min
    PortfolioRiskManager prm(config);
    prm.update_balance(1000.0);

    // Trigger circuit breaker
    prm.on_trade_closed(-60.0);

    // With cooldown = 0 minutes, the breaker should release
    Positions positions;
    auto decision = prm.check_entry(
        "BTCUSDT", "15", 50000.0, 0.01, 10, 1000.0, positions);

    EXPECT_TRUE(decision.allowed);
}

// ── Max total positions ──
TEST(PortfolioRisk, MaxTotalPositionsExceeded) {
    auto config = make_config(2);  // max 2 total positions
    PortfolioRiskManager prm(config);

    Positions positions;
    positions["BTCUSDT_15_1"] = make_pos("BTCUSDT", "15", 50000, 0.01);
    positions["ETHUSDT_15_1"] = make_pos("ETHUSDT", "15", 3000, 0.1);

    auto decision = prm.check_entry(
        "SOLUSDT", "15", 150.0, 1.0, 10, 10000.0, positions);

    EXPECT_FALSE(decision.allowed);
    EXPECT_EQ(decision.check_failed, "max_positions");
}

// ── TF position limit ──
TEST(PortfolioRisk, TFPositionLimitExceeded) {
    // max_positions_per_tf for "5" is 3
    auto config = make_config();
    PortfolioRiskManager prm(config);

    Positions positions;
    positions["BTC_5_1"] = make_pos("BTCUSDT", "5", 50000, 0.01);
    positions["ETH_5_1"] = make_pos("ETHUSDT", "5", 3000, 0.1);
    positions["SOL_5_1"] = make_pos("SOLUSDT", "5", 150, 1.0);

    auto decision = prm.check_entry(
        "AVAXUSDT", "5", 40.0, 10.0, 10, 100000.0, positions);

    EXPECT_FALSE(decision.allowed);
    EXPECT_EQ(decision.check_failed, "tf_position_limit");
}

TEST(PortfolioRisk, TFPositionLimitDifferentTFAllowed) {
    auto config = make_config();
    PortfolioRiskManager prm(config);

    // Fill up TF "5" (limit=3)
    Positions positions;
    positions["BTC_5_1"] = make_pos("BTCUSDT", "5", 50000, 0.01);
    positions["ETH_5_1"] = make_pos("ETHUSDT", "5", 3000, 0.1);
    positions["SOL_5_1"] = make_pos("SOLUSDT", "5", 150, 1.0);

    // But TF "15" (limit=5) still has room
    auto decision = prm.check_entry(
        "AVAXUSDT", "15", 40.0, 10.0, 10, 100000.0, positions);

    EXPECT_TRUE(decision.allowed);
}

// ── TF exposure limit ──
TEST(PortfolioRisk, TFExposureLimitExceeded) {
    // tf_exposure_pct for "5" = 20%, balance = 100 -> max margin = 20
    auto config = make_config();
    PortfolioRiskManager prm(config);

    Positions positions;
    // Existing position: margin = 100 * 1.0 / 10 = 10
    positions["BTC_5_1"] = make_pos("BTCUSDT", "5", 100, 1.0, 10);

    // New order: margin = 200 * 1.0 / 10 = 20 -> total = 30 > 20
    auto decision = prm.check_entry(
        "ETHUSDT", "5", 200.0, 1.0, 10, 100.0, positions);

    EXPECT_FALSE(decision.allowed);
    EXPECT_EQ(decision.check_failed, "tf_exposure");
}

// ── Correlation risk limit ──
TEST(PortfolioRisk, CorrelationRiskExceeded) {
    // corr_factor=0.7, max_corr_risk_pct=10% -> corr_limit = 0.1 * balance
    // total_margin_risk = sum(entry*qty/lev) for all positions + new
    // corr_risk = total_margin_risk * 0.7
    auto config = make_config(50, 0.7, 10.0);  // max_corr_risk_pct = 10%
    PortfolioRiskManager prm(config);

    Positions positions;
    // Existing on TF "15": margin = 100 * 0.5 / 10 = 5
    positions["BTC_15_1"] = make_pos("BTCUSDT", "15", 100, 0.5, 10);

    // New on TF "60" (exposure_pct=40%): notional = 100 * 0.5 = 50, margin = 50/10 = 5
    // total_margin_risk = 10, corr_risk = 10 * 0.7 = 7
    // corr_limit = 10 * 10% = 1 -> 7 > 1 -> blocked
    // TF "60" margin = 5 < 10 * 40% = 4... still too tight.
    // Use balance = 10: corr_limit = 10 * 10% = 1 -> 7 > 1 -> blocked
    // TF "60" exposure = 10 * 40% = 4 -> 5 > 4... also fails.
    // Use a TF not in config (defaults to 30% exposure)
    // New on TF "120" (no config, default 30%): margin = 5, tf_max = 10 * 30% = 3 -> 5 > 3
    // Need larger balance so TF exposure passes but corr fails.
    // balance=100: tf_max = 100 * 40% = 40 -> 5 < 40 (pass)
    // corr_limit = 100 * 10% = 10 -> corr_risk = 10 * 0.7 = 7 < 10 (pass!)
    // Need more margin. Use balance=10, bigger positions on TF "60"
    // Let me just use high balance and more positions to get corr_risk high.
    positions["ETH_60_1"] = make_pos("ETHUSDT", "60", 200, 1.0, 10);  // margin=20
    positions["SOL_60_2"] = make_pos("SOLUSDT", "60", 200, 1.0, 10);  // margin=20
    // existing margins: 5 + 20 + 20 = 45
    // New: notional = 200 * 1.0 = 200, margin = 200/10 = 20
    // total_margin_risk = 65, corr_risk = 65 * 0.7 = 45.5
    // corr_limit = 100 * 10% = 10 -> 45.5 > 10 -> blocked
    // TF "60" existing margin = 40, new margin = 20 -> total = 60 > 100*40% = 40 -> tf_exposure first!
    // Use different TF for new entry. TF "15" existing = 5, new = 20 -> 25 < 100*30% = 30 (pass)
    auto decision = prm.check_entry(
        "AVAXUSDT", "15", 200.0, 1.0, 10, 100.0, positions);

    EXPECT_FALSE(decision.allowed);
    EXPECT_EQ(decision.check_failed, "correlation_risk");
}

// ── Margin limit (90%) ──
TEST(PortfolioRisk, MarginLimit90Percent) {
    auto config = make_config(50, 0.7, 100.0);  // high corr limit to pass
    PortfolioRiskManager prm(config);

    Positions positions;
    // Existing: margin = 10000 * 1.0 / 10 = 1000
    // That's already 100% of a 1000 balance
    positions["BTC_15_1"] = make_pos("BTCUSDT", "15", 10000, 1.0, 10);

    // New: margin = 100 * 1.0 / 10 = 10
    // total = 1010 > 1000 * 0.90 = 900 -> blocked
    auto decision = prm.check_entry(
        "ETHUSDT", "60", 100.0, 1.0, 10, 1000.0, positions);

    EXPECT_FALSE(decision.allowed);
    EXPECT_EQ(decision.check_failed, "margin_limit");
}

TEST(PortfolioRisk, MarginUnder90Allowed) {
    auto config = make_config(50, 0.7, 100.0);
    PortfolioRiskManager prm(config);

    Positions positions;
    // margin = 100 * 1.0 / 10 = 10 -> 10 < 10000 * 0.90 = 9000
    positions["BTC_15_1"] = make_pos("BTCUSDT", "15", 100, 1.0, 10);

    auto decision = prm.check_entry(
        "ETHUSDT", "15", 100.0, 1.0, 10, 10000.0, positions);

    EXPECT_TRUE(decision.allowed);
}

// ── Portfolio state ──
TEST(PortfolioRisk, GetStateComputation) {
    auto config = make_config();
    PortfolioRiskManager prm(config);

    Positions positions;
    positions["BTC_15_1"] = make_pos("BTCUSDT", "15", 50000, 0.01, 10);
    positions["ETH_5_1"]  = make_pos("ETHUSDT", "5", 3000, 0.1, 10);

    auto state = prm.get_state(10000.0, positions, 12000.0);

    EXPECT_EQ(state.total_positions, 2);
    EXPECT_GT(state.total_notional, 0.0);
    EXPECT_GT(state.total_margin_used, 0.0);
    // Drawdown: (12000 - 10000) / 12000 * 100 = 16.67%
    EXPECT_NEAR(state.current_drawdown_pct, 16.67, 0.1);
}

// ── Check stats tracking ──
TEST(PortfolioRisk, CheckStatsTracking) {
    auto config = make_config(1);  // max 1 position
    PortfolioRiskManager prm(config);

    Positions empty;
    Positions one;
    one["BTC_15_1"] = make_pos("BTCUSDT", "15", 50000, 0.01);

    prm.check_entry("BTCUSDT", "15", 50000, 0.01, 10, 10000, empty);  // pass
    prm.check_entry("ETHUSDT", "15", 3000, 0.1, 10, 10000, one);       // blocked

    auto stats = prm.get_check_stats();
    EXPECT_EQ(stats["total_checks"].get<int>(), 2);
    EXPECT_EQ(stats["passed"].get<int>(), 1);
    EXPECT_EQ(stats["blocked"].get<int>(), 1);
}

// ── Empty positions -- all checks pass ──
TEST(PortfolioRisk, EmptyPositionsAllPass) {
    auto config = make_config();
    PortfolioRiskManager prm(config);

    Positions empty;
    auto decision = prm.check_entry(
        "BTCUSDT", "15", 50000.0, 0.01, 10, 10000.0, empty);

    EXPECT_TRUE(decision.allowed);
}

// ── Unknown TF defaults ──
TEST(PortfolioRisk, UnknownTFUsesDefault) {
    auto config = make_config();
    PortfolioRiskManager prm(config);

    Positions positions;
    // TF "240" not in config -> defaults to limit=10, exposure_pct=30%
    auto decision = prm.check_entry(
        "BTCUSDT", "240", 50000.0, 0.01, 10, 10000.0, positions);

    EXPECT_TRUE(decision.allowed);
}

// ── Hard notional cap ──
TEST(PortfolioRisk, HardNotionalCapExceeded) {
    auto config = make_config();
    PortfolioRiskManager prm(config);

    Positions empty;
    // notional = 1000 * 1.0 = 1000 > 500
    auto decision = prm.check_entry(
        "BTCUSDT", "15", 1000.0, 1.0, 10, 100000.0, empty);

    EXPECT_FALSE(decision.allowed);
    EXPECT_EQ(decision.check_failed, "hard_notional_cap");
}

TEST(PortfolioRisk, HardNotionalCapExactlyAtLimit) {
    auto config = make_config();
    PortfolioRiskManager prm(config);

    Positions empty;
    // notional = 500 * 1.0 = 500 == HARD_MAX_NOTIONAL (not >, so allowed)
    auto decision = prm.check_entry(
        "BTCUSDT", "15", 500.0, 1.0, 10, 100000.0, empty);

    EXPECT_TRUE(decision.allowed);
}

// ── SL beyond liquidation (LONG) ──
TEST(PortfolioRisk, SLBeyondLiquidationLong) {
    auto config = make_config();
    PortfolioRiskManager prm(config);

    Positions empty;
    // LONG 20x: liq_est = 100 * (1 - 0.9/20) = 100 * 0.955 = 95.5
    // SL at 90 < 95.5 -> blocked
    auto decision = prm.check_entry(
        "SOLUSDT", "15", 100.0, 1.0, 20, 10000.0, empty, 90.0, "long");

    EXPECT_FALSE(decision.allowed);
    EXPECT_EQ(decision.check_failed, "sl_beyond_liquidation");
}

TEST(PortfolioRisk, SLWithinLiquidationLong) {
    auto config = make_config();
    PortfolioRiskManager prm(config);

    Positions empty;
    // LONG 20x: liq_est = 100 * (1 - 0.9/20) = 95.5
    // SL at 97 > 95.5 -> allowed
    auto decision = prm.check_entry(
        "SOLUSDT", "15", 100.0, 1.0, 20, 10000.0, empty, 97.0, "long");

    EXPECT_TRUE(decision.allowed);
}

// ── SL beyond liquidation (SHORT) ──
TEST(PortfolioRisk, SLBeyondLiquidationShort) {
    auto config = make_config();
    PortfolioRiskManager prm(config);

    Positions empty;
    // SHORT 20x: liq_est = 100 * (1 + 0.9/20) = 100 * 1.045 = 104.5
    // SL at 110 > 104.5 -> blocked
    auto decision = prm.check_entry(
        "SOLUSDT", "15", 100.0, 1.0, 20, 10000.0, empty, 110.0, "short");

    EXPECT_FALSE(decision.allowed);
    EXPECT_EQ(decision.check_failed, "sl_beyond_liquidation");
}

// ── Configurable margin limit ──
TEST(PortfolioRisk, ConfigurableMarginLimit) {
    // Set max_margin_usage_pct to 50%
    json config = {
        {"portfolio_risk", {
            {"max_positions_total", 50},
            {"max_positions_per_tf", {{"15", 5}}},
            {"tf_exposure_pct", {{"15", 80.0}}},
            {"correlation_factor", 0.1},
            {"max_correlated_risk_pct", 100.0},
            {"max_margin_usage_pct", 50.0},
            {"circuit_breaker", {
                {"daily_loss_pct", 50.0},
                {"weekly_loss_pct", 100.0},
                {"cooldown_minutes", 60}
            }}
        }}
    };
    PortfolioRiskManager prm(config);

    Positions positions;
    // Existing: margin = 400 * 1.0 / 10 = 40
    positions["BTC_15_1"] = make_pos("BTCUSDT", "15", 400, 1.0, 10);

    // New: margin = 200 * 1.0 / 10 = 20 -> total = 60 > 100 * 0.50 = 50
    auto decision = prm.check_entry(
        "ETHUSDT", "15", 200.0, 1.0, 10, 100.0, positions);

    EXPECT_FALSE(decision.allowed);
    EXPECT_EQ(decision.check_failed, "margin_limit");
}

// ── Rapid loss circuit breaker ──
TEST(PortfolioRisk, RapidLossCircuitBreaker) {
    json config = {
        {"portfolio_risk", {
            {"max_positions_total", 50},
            {"max_positions_per_tf", {{"15", 5}}},
            {"tf_exposure_pct", {{"15", 80.0}}},
            {"correlation_factor", 0.1},
            {"max_correlated_risk_pct", 100.0},
            {"rapid_loss_pct", 2.0},
            {"rapid_loss_window_minutes", 10},
            {"circuit_breaker", {
                {"daily_loss_pct", 50.0},
                {"weekly_loss_pct", 100.0},
                {"cooldown_minutes", 9999}
            }}
        }}
    };
    PortfolioRiskManager prm(config);
    prm.update_balance(1000.0);

    // Rapid losses totaling > 2% of 1000 = 20 within 10 minutes
    prm.on_trade_closed(-12.0);
    prm.on_trade_closed(-12.0);  // total = -24 > -20

    Positions empty;
    auto decision = prm.check_entry(
        "BTCUSDT", "15", 100.0, 1.0, 10, 1000.0, empty);

    EXPECT_FALSE(decision.allowed);
    EXPECT_EQ(decision.check_failed, "circuit_breaker");
}

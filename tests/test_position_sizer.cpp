// ============================================================================
// tests/test_position_sizer.cpp -- PositionSizer::calc_size() unit tests
// ============================================================================
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "risk/position_sizer.hpp"

using namespace hft;
using json = nlohmann::json;

// Helper: create a default config
static json make_config(double base_pct = 30.0, double max_pct = 50.0,
                        double min_usdt = 5.0, double max_usdt = 100.0,
                        double kelly_frac = 0.25, double max_risk_pct = 5.0)
{
    return json{
        {"position_sizing", {
            {"base_trade_pct", base_pct},
            {"max_trade_pct", max_pct},
            {"min_trade_usdt", min_usdt},
            {"max_trade_usdt", max_usdt},
            {"kelly_fraction", kelly_frac},
            {"max_risk_per_trade_pct", max_risk_pct},
            {"drawdown_levels", json::array({
                {{"threshold", 10.0}, {"multiplier", 1.0}},
                {{"threshold", 20.0}, {"multiplier", 0.75}},
                {{"threshold", 30.0}, {"multiplier", 0.50}},
                {{"threshold", 50.0}, {"multiplier", 0.25}},
                {{"threshold", 100.0}, {"multiplier", 0.0}}
            })}
        }}
    };
}

// Helper: create a SymbolScore for testing
static SymbolScore make_score(const std::string& tier = "B",
                              double size_mult = 1.0,
                              int max_lev = 20,
                              bool data_sufficient = true,
                              double win_rate = 0.6,
                              double avg_win = 0.05,
                              double avg_loss = 0.03)
{
    SymbolScore s;
    s.tier = tier;
    s.size_multiplier = size_mult;
    s.max_leverage = max_lev;
    s.data_sufficient = data_sufficient;
    s.win_rate = win_rate;
    s.avg_win = avg_win;
    s.avg_loss = avg_loss;
    return s;
}

// ── Normal case with valid inputs ──
TEST(PositionSizer, NormalCaseValidInputs) {
    auto config = make_config();
    PositionSizer sizer(config);

    auto result = sizer.calc_size(
        /*balance=*/1000.0, /*symbol=*/"BTCUSDT", /*price=*/50000.0,
        /*sl_price=*/49000.0, /*leverage=*/10,
        /*score=*/nullptr, /*current_dd_pct=*/0.0);

    EXPECT_GT(result.qty, 0.0);
    EXPECT_GT(result.usdt_amount, 0.0);
    EXPECT_LE(result.usdt_amount, 100.0);  // max_trade_usdt = 100
    EXPECT_GE(result.usdt_amount, 5.0);    // min_trade_usdt = 5
    EXPECT_EQ(result.leverage, 10);
}

// ── Zero balance ──
TEST(PositionSizer, ZeroBalance) {
    auto config = make_config();
    PositionSizer sizer(config);

    auto result = sizer.calc_size(
        0.0, "BTCUSDT", 50000.0, 49000.0, 10, nullptr, 0.0);

    // Available < min_usdt -> insufficient
    EXPECT_DOUBLE_EQ(result.qty, 0.0);
    EXPECT_EQ(result.method_used, "insufficient");
}

// ── SL == entry price (division by zero protection) ──
TEST(PositionSizer, SLEqualsEntryPrice) {
    auto config = make_config();
    PositionSizer sizer(config);

    // sl_price == price -> sl_dist = 0 -> should not crash
    auto result = sizer.calc_size(
        1000.0, "BTCUSDT", 50000.0, 50000.0, 10, nullptr, 0.0);

    // Should still produce a valid result (falls back to balance_pct)
    EXPECT_GT(result.qty, 0.0);
    EXPECT_EQ(result.method_used, "balance_pct");
}

// ── Kelly fraction calculation ──
TEST(PositionSizer, KellyFractionWithData) {
    auto config = make_config();
    PositionSizer sizer(config);

    auto score = make_score("A", 1.2, 20, true, 0.6, 0.05, 0.03);
    auto result = sizer.calc_size(
        1000.0, "BTCUSDT", 50000.0, 49000.0, 10, &score, 0.0);

    // Kelly f = wr - (1-wr)/R = 0.6 - 0.4/(0.05/0.03) = 0.6 - 0.24 = 0.36
    // Scaled by kelly_frac (0.25) -> 0.09, capped by max_risk_pct (0.05)
    EXPECT_GT(result.kelly_fraction, 0.0);
    EXPECT_LE(result.kelly_fraction, 0.05);  // capped at max_risk_pct
    EXPECT_GT(result.kelly_usdt, 0.0);
    // Method should be kelly_risk_hybrid when data sufficient
    EXPECT_EQ(result.method_used, "kelly_risk_hybrid");
}

TEST(PositionSizer, KellyNotUsedWithoutData) {
    auto config = make_config();
    PositionSizer sizer(config);

    auto score = make_score("C", 0.5, 10, false, 0.0, 0.0, 0.0);
    auto result = sizer.calc_size(
        1000.0, "BTCUSDT", 50000.0, 49000.0, 10, &score, 0.0);

    // data_sufficient=false -> no kelly
    EXPECT_EQ(result.method_used, "risk_per_trade");
}

// ── Drawdown multiplier application ──
TEST(PositionSizer, DrawdownMultiplierReducesSize) {
    auto config = make_config();
    PositionSizer sizer(config);

    auto result_no_dd = sizer.calc_size(
        1000.0, "BTCUSDT", 50000.0, 0.0, 10, nullptr, 0.0);
    auto result_dd15 = sizer.calc_size(
        1000.0, "BTCUSDT", 50000.0, 0.0, 10, nullptr, 15.0);

    // DD 15% -> multiplier 0.75, so size should be smaller
    EXPECT_LT(result_dd15.usdt_amount, result_no_dd.usdt_amount);
    EXPECT_DOUBLE_EQ(result_dd15.drawdown_multiplier, 0.75);
}

TEST(PositionSizer, DrawdownHaltsTrading) {
    auto config = make_config();
    PositionSizer sizer(config);

    // DD > 50% -> multiplier 0.0 (threshold 100 at 0.0)
    auto result = sizer.calc_size(
        1000.0, "BTCUSDT", 50000.0, 0.0, 10, nullptr, 55.0);

    EXPECT_DOUBLE_EQ(result.qty, 0.0);
    EXPECT_EQ(result.method_used, "dd_stop");
}

// ── Min/max trade size clamping ──
TEST(PositionSizer, MinTradeSizeClamping) {
    // Very small balance -> should still get min_usdt
    auto config = make_config(30.0, 50.0, 5.0, 100.0);
    PositionSizer sizer(config);

    auto result = sizer.calc_size(
        20.0, "BTCUSDT", 50000.0, 0.0, 10, nullptr, 0.0);

    // Balance 20, base_pct 30% = 6 USDT, min = 5, should work
    EXPECT_GE(result.usdt_amount, 5.0);
}

TEST(PositionSizer, MaxTradeSizeClamping) {
    // Large balance but max_trade_usdt = 100
    auto config = make_config(30.0, 50.0, 5.0, 100.0);
    PositionSizer sizer(config);

    auto result = sizer.calc_size(
        10000.0, "BTCUSDT", 50000.0, 0.0, 10, nullptr, 0.0);

    EXPECT_LE(result.usdt_amount, 100.0);  // hard cap
}

TEST(PositionSizer, MaxPctClamping) {
    // max_trade_pct = 50%, balance = 100 -> max = 50 USDT
    auto config = make_config(30.0, 50.0, 5.0, 10000.0);  // high max_usdt
    PositionSizer sizer(config);

    auto result = sizer.calc_size(
        100.0, "BTCUSDT", 50000.0, 0.0, 10, nullptr, 0.0);

    EXPECT_LE(result.usdt_amount, 50.0);  // 50% of 100
}

// ── Different tier multipliers ──
TEST(PositionSizer, TierMultiplierAffectsSize) {
    auto config = make_config();
    PositionSizer sizer(config);

    auto score_s = make_score("S", 1.5, 20, false);
    auto score_d = make_score("D", 0.3, 5, false);

    auto result_s = sizer.calc_size(
        1000.0, "BTCUSDT", 50000.0, 0.0, 10, &score_s, 0.0);
    auto result_d = sizer.calc_size(
        1000.0, "BTCUSDT", 50000.0, 0.0, 10, &score_d, 0.0);

    // S-tier (1.5x) should produce larger size than D-tier (0.3x)
    EXPECT_GT(result_s.usdt_amount, result_d.usdt_amount);
    EXPECT_DOUBLE_EQ(result_s.tier_multiplier, 1.5);
    EXPECT_DOUBLE_EQ(result_d.tier_multiplier, 0.3);
}

// ── X-tier blacklist ──
TEST(PositionSizer, XTierBlocked) {
    auto config = make_config();
    PositionSizer sizer(config);

    auto score_x = make_score("X", 0.0, 5, false);
    auto result = sizer.calc_size(
        1000.0, "BTCUSDT", 50000.0, 0.0, 10, &score_x, 0.0);

    EXPECT_DOUBLE_EQ(result.qty, 0.0);
    EXPECT_EQ(result.method_used, "blocked");
}

// ── Leverage capped by score ──
TEST(PositionSizer, LeverageCappedByScore) {
    auto config = make_config();
    PositionSizer sizer(config);

    auto score = make_score("C", 0.5, 5, false);  // max_leverage = 5
    auto result = sizer.calc_size(
        1000.0, "BTCUSDT", 50000.0, 49000.0, 20, &score, 0.0);

    // Requested 20x but score limits to 5x
    EXPECT_EQ(result.leverage, 5);
}

// ── Used margin reduces available balance ──
TEST(PositionSizer, UsedMarginReducesAvailable) {
    auto config = make_config();
    PositionSizer sizer(config);

    auto result_no_margin = sizer.calc_size(
        1000.0, "BTCUSDT", 50000.0, 0.0, 10, nullptr, 0.0, 0.0);
    auto result_with_margin = sizer.calc_size(
        1000.0, "BTCUSDT", 50000.0, 0.0, 10, nullptr, 0.0, 500.0);

    // With 500 USDT used margin, available is only 500 -> smaller size
    EXPECT_LT(result_with_margin.usdt_amount, result_no_margin.usdt_amount);
}

// ── Risk-per-trade sizing with SL ──
TEST(PositionSizer, RiskPerTradeWithSL) {
    auto config = make_config();
    PositionSizer sizer(config);

    // No score data -> risk_per_trade method when SL provided
    auto result = sizer.calc_size(
        1000.0, "BTCUSDT", 50000.0, 49000.0, 10, nullptr, 0.0);

    EXPECT_EQ(result.method_used, "risk_per_trade");
    EXPECT_GT(result.risk_usdt, 0.0);
}

// ── Quantity conversion ──
TEST(PositionSizer, QuantityCorrectlyComputed) {
    auto config = make_config(30.0, 50.0, 5.0, 1000.0);
    PositionSizer sizer(config);

    auto result = sizer.calc_size(
        1000.0, "BTCUSDT", 100.0, 0.0, 10, nullptr, 0.0);

    // qty = usdt_amount / price
    EXPECT_NEAR(result.qty, result.usdt_amount / 100.0, 0.01);
}

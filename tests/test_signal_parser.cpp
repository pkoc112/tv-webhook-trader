// ============================================================================
// tests/test_signal_parser.cpp -- WebhookSignal::from_json() unit tests
// ============================================================================
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include "webhook/signal_types.hpp"

using namespace hft;
using json = nlohmann::json;

// Helper: build a minimal valid SFX entry JSON string
static std::string make_entry_json(const std::string& alert = "buy",
                                   const std::string& direction = "bull",
                                   double close = 100.0,
                                   const std::string& ticker = "BINANCE:BTCUSDT.P")
{
    json j;
    j["algorithm"]        = "SFX";
    j["ticker"]           = ticker;
    j["ticker_full"]      = ticker;
    j["unix_time"]        = 1710000000;
    j["timestamp"]        = "2026-03-10T00:00:00Z";
    j["currency"]         = "USDT";
    j["timeframe"]        = "15";
    j["type"]             = "crypto";
    j["alert"]            = alert;
    j["signal_direction"] = direction;
    j["current_rating"]   = 5;
    j["close"]            = close;
    j["tp1"]              = 110.0;
    j["tp2"]              = 120.0;
    j["tp3"]              = 130.0;
    j["sl"]               = 90.0;
    j["token"]            = "test_token";
    return j.dump();
}

// ── Valid entry signal with all fields ──
TEST(SignalParser, ValidEntryBuyAllFields) {
    auto body = make_entry_json("buy", "bull", 100.0);
    auto opt = WebhookSignal::from_json(body);
    ASSERT_TRUE(opt.has_value());

    auto& sig = *opt;
    EXPECT_EQ(sig.sig_type, SignalType::Entry);
    EXPECT_EQ(sig.action, "buy");
    EXPECT_EQ(sig.trade_side, "open");
    EXPECT_EQ(sig.symbol, "BTCUSDT");  // .P stripped, prefix stripped
    EXPECT_DOUBLE_EQ(sig.price, 100.0);
    EXPECT_DOUBLE_EQ(sig.tp1, 110.0);
    EXPECT_DOUBLE_EQ(sig.tp2, 120.0);
    EXPECT_DOUBLE_EQ(sig.tp3, 130.0);
    EXPECT_DOUBLE_EQ(sig.sl, 90.0);
    EXPECT_EQ(sig.token, "test_token");
    EXPECT_EQ(sig.algorithm, "SFX");
    EXPECT_EQ(sig.timeframe, "15");
    EXPECT_EQ(sig.signal_direction, "bull");
    EXPECT_EQ(sig.current_rating, 5);
    EXPECT_EQ(sig.priority, 2);  // 15m -> priority 2
    EXPECT_TRUE(sig.is_valid());
    EXPECT_EQ(sig.get_side(), Side::Buy);
    EXPECT_EQ(sig.get_trade_side(), TradeSide::Open);
}

TEST(SignalParser, ValidEntrySell) {
    auto body = make_entry_json("sell", "bear", 50000.0);
    auto opt = WebhookSignal::from_json(body);
    ASSERT_TRUE(opt.has_value());

    auto& sig = *opt;
    EXPECT_EQ(sig.sig_type, SignalType::Entry);
    EXPECT_EQ(sig.action, "sell");
    EXPECT_EQ(sig.trade_side, "open");
    EXPECT_EQ(sig.get_side(), Side::Sell);
    EXPECT_EQ(sig.get_hold_side(), "short");
}

// ── Entry with NaN SL (should parse as 0) ──
TEST(SignalParser, EntryWithNanSL) {
    json j;
    j["algorithm"]        = "SFX";
    j["ticker"]           = "BINANCE:ETHUSDT.P";
    j["alert"]            = "buy";
    j["signal_direction"] = "bull";
    j["close"]            = 3000.0;
    j["sl"]               = "NaN";
    j["tp1"]              = 3100.0;

    auto opt = WebhookSignal::from_json(j.dump());
    ASSERT_TRUE(opt.has_value());
    EXPECT_DOUBLE_EQ(opt->sl, 0.0);
    EXPECT_FALSE(opt->has_sl());
}

TEST(SignalParser, EntryWithNanString) {
    json j;
    j["algorithm"]        = "SFX";
    j["ticker"]           = "BINANCE:ETHUSDT.P";
    j["alert"]            = "buy";
    j["signal_direction"] = "bull";
    j["close"]            = 3000.0;
    j["sl"]               = "nan";

    auto opt = WebhookSignal::from_json(j.dump());
    ASSERT_TRUE(opt.has_value());
    EXPECT_DOUBLE_EQ(opt->sl, 0.0);
}

// ── Entry with missing fields ──
TEST(SignalParser, EntryMissingOptionalFields) {
    json j;
    j["algorithm"] = "SFX";
    j["ticker"]    = "BINANCE:SOLUSDT.P";
    j["alert"]     = "buy";
    j["signal_direction"] = "bull";
    j["close"]     = 150.0;
    // No tp1, tp2, tp3, sl, token

    auto opt = WebhookSignal::from_json(j.dump());
    ASSERT_TRUE(opt.has_value());
    EXPECT_EQ(opt->symbol, "SOLUSDT");
    EXPECT_DOUBLE_EQ(opt->tp1, 0.0);
    EXPECT_DOUBLE_EQ(opt->tp2, 0.0);
    EXPECT_DOUBLE_EQ(opt->tp3, 0.0);
    EXPECT_DOUBLE_EQ(opt->sl, 0.0);
    EXPECT_FALSE(opt->has_tp1());
    EXPECT_FALSE(opt->has_tp2());
    EXPECT_FALSE(opt->has_tp3());
    EXPECT_FALSE(opt->has_sl());
    EXPECT_EQ(opt->token, "");
}

// ── TP1/TP2/TP3 signals ──
TEST(SignalParser, TP1SignalBull) {
    json j;
    j["algorithm"]        = "SFX";
    j["ticker"]           = "BINANCE:BTCUSDT.P";
    j["alert"]            = "TP1";
    j["signal_direction"] = "bull";
    j["close"]            = 110.0;
    j["entry_price"]      = 100.0;

    auto opt = WebhookSignal::from_json(j.dump());
    ASSERT_TRUE(opt.has_value());
    EXPECT_EQ(opt->sig_type, SignalType::TP);
    EXPECT_EQ(opt->tp_level, "TP1");
    EXPECT_EQ(opt->trade_side, "close");
    EXPECT_EQ(opt->action, "sell");  // bull TP -> sell to close long
    EXPECT_DOUBLE_EQ(opt->entry_price, 100.0);
}

TEST(SignalParser, TP2SignalBear) {
    json j;
    j["algorithm"]        = "SFX";
    j["ticker"]           = "BINANCE:BTCUSDT.P";
    j["alert"]            = "TP2";
    j["signal_direction"] = "bear";
    j["close"]            = 90.0;

    auto opt = WebhookSignal::from_json(j.dump());
    ASSERT_TRUE(opt.has_value());
    EXPECT_EQ(opt->sig_type, SignalType::TP);
    EXPECT_EQ(opt->tp_level, "TP2");
    EXPECT_EQ(opt->action, "buy");  // bear TP -> buy to close short
}

TEST(SignalParser, TP3Signal) {
    json j;
    j["algorithm"]        = "SFX";
    j["ticker"]           = "BINANCE:ETHUSDT.P";
    j["alert"]            = "TP3";
    j["signal_direction"] = "bull";
    j["close"]            = 4000.0;

    auto opt = WebhookSignal::from_json(j.dump());
    ASSERT_TRUE(opt.has_value());
    EXPECT_EQ(opt->sig_type, SignalType::TP);
    EXPECT_EQ(opt->tp_level, "TP3");
    EXPECT_EQ(opt->trade_side, "close");
}

// ── SL signal (bull and bear) ──
TEST(SignalParser, SLSignalBull) {
    json j;
    j["algorithm"]        = "SFX";
    j["ticker"]           = "BINANCE:BTCUSDT.P";
    j["alert"]            = "SL";
    j["signal_direction"] = "bull";
    j["close"]            = 88.0;

    auto opt = WebhookSignal::from_json(j.dump());
    ASSERT_TRUE(opt.has_value());
    EXPECT_EQ(opt->sig_type, SignalType::SL);
    EXPECT_EQ(opt->trade_side, "close");
    EXPECT_EQ(opt->action, "sell");  // bull SL -> sell to close long
}

TEST(SignalParser, SLSignalBear) {
    json j;
    j["algorithm"]        = "SFX";
    j["ticker"]           = "BINANCE:BTCUSDT.P";
    j["alert"]            = "STOP_LOSS";
    j["signal_direction"] = "bear";
    j["close"]            = 112.0;

    auto opt = WebhookSignal::from_json(j.dump());
    ASSERT_TRUE(opt.has_value());
    EXPECT_EQ(opt->sig_type, SignalType::SL);
    EXPECT_EQ(opt->action, "buy");  // bear SL -> buy to close short
}

TEST(SignalParser, AutoSLAlias) {
    json j;
    j["algorithm"]        = "SFX";
    j["ticker"]           = "BINANCE:BTCUSDT.P";
    j["alert"]            = "AUTO_SL";
    j["signal_direction"] = "bull";
    j["close"]            = 85.0;

    auto opt = WebhookSignal::from_json(j.dump());
    ASSERT_TRUE(opt.has_value());
    EXPECT_EQ(opt->sig_type, SignalType::SL);
}

// ── ReEntry signal ──
TEST(SignalParser, ReEntrySignalBull) {
    json j;
    j["algorithm"]        = "SFX";
    j["ticker"]           = "BINANCE:BTCUSDT.P";
    j["alert"]            = "RE_ENTRY";
    j["signal_direction"] = "bull";
    j["close"]            = 105.0;

    auto opt = WebhookSignal::from_json(j.dump());
    ASSERT_TRUE(opt.has_value());
    EXPECT_EQ(opt->sig_type, SignalType::ReEntry);
    EXPECT_EQ(opt->action, "buy");
    EXPECT_EQ(opt->trade_side, "open");
    EXPECT_TRUE(opt->is_valid());
}

TEST(SignalParser, ReEntrySignalBear) {
    json j;
    j["algorithm"]        = "SFX";
    j["ticker"]           = "BINANCE:BTCUSDT.P";
    j["alert"]            = "RE";
    j["signal_direction"] = "bear";
    j["close"]            = 95.0;

    auto opt = WebhookSignal::from_json(j.dump());
    ASSERT_TRUE(opt.has_value());
    EXPECT_EQ(opt->sig_type, SignalType::ReEntry);
    EXPECT_EQ(opt->action, "sell");
}

// ── Close signal (generic format) ──
TEST(SignalParser, GenericCloseSignal) {
    json j;
    j["action"]     = "sell";
    j["symbol"]     = "BTCUSDT";
    j["trade_side"] = "close";
    j["price"]      = 105.0;
    j["qty"]        = 0.01;

    auto opt = WebhookSignal::from_json(j.dump());
    ASSERT_TRUE(opt.has_value());
    EXPECT_EQ(opt->action, "sell");
    EXPECT_EQ(opt->trade_side, "close");
    EXPECT_EQ(opt->get_trade_side(), TradeSide::Close);
    EXPECT_DOUBLE_EQ(opt->price, 105.0);
    EXPECT_DOUBLE_EQ(opt->size, 0.01);
}

// ── Invalid JSON ──
TEST(SignalParser, InvalidJSON) {
    auto opt = WebhookSignal::from_json("{not valid json at all");
    EXPECT_FALSE(opt.has_value());
}

TEST(SignalParser, InvalidJSONGarbage) {
    auto opt = WebhookSignal::from_json("hello world");
    EXPECT_FALSE(opt.has_value());
}

// ── Empty body ──
TEST(SignalParser, EmptyBody) {
    auto opt = WebhookSignal::from_json("");
    EXPECT_FALSE(opt.has_value());
}

// ── USDC symbol detection ──
TEST(SignalParser, USDCSymbol) {
    json j;
    j["algorithm"]        = "SFX";
    j["ticker"]           = "BINANCE:BTCUSDC.P";
    j["alert"]            = "buy";
    j["signal_direction"] = "bull";
    j["close"]            = 100.0;

    auto opt = WebhookSignal::from_json(j.dump());
    ASSERT_TRUE(opt.has_value());
    EXPECT_EQ(opt->symbol, "BTCUSDC");
}

TEST(SignalParser, SymbolWithoutExchangePrefix) {
    json j;
    j["algorithm"]        = "SFX";
    j["ticker"]           = "ETHUSDT";
    j["alert"]            = "buy";
    j["signal_direction"] = "bull";
    j["close"]            = 3000.0;

    auto opt = WebhookSignal::from_json(j.dump());
    ASSERT_TRUE(opt.has_value());
    EXPECT_EQ(opt->symbol, "ETHUSDT");
}

// ── Signal fingerprint uniqueness ──
TEST(SignalParser, FingerprintUniqueness) {
    auto body1 = make_entry_json("buy", "bull", 100.0, "BINANCE:BTCUSDT.P");
    auto body2 = make_entry_json("buy", "bull", 100.0, "BINANCE:ETHUSDT.P");

    auto sig1 = WebhookSignal::from_json(body1);
    auto sig2 = WebhookSignal::from_json(body2);
    ASSERT_TRUE(sig1.has_value());
    ASSERT_TRUE(sig2.has_value());

    // Different symbols -> different fingerprints
    EXPECT_NE(sig1->fingerprint(), sig2->fingerprint());
}

TEST(SignalParser, FingerprintSameInputDeterministic) {
    auto body = make_entry_json("buy", "bull", 100.0);
    auto sig1 = WebhookSignal::from_json(body);
    auto sig2 = WebhookSignal::from_json(body);
    ASSERT_TRUE(sig1.has_value());
    ASSERT_TRUE(sig2.has_value());

    // Same input -> same fingerprint (deterministic hash)
    EXPECT_EQ(sig1->fingerprint(), sig2->fingerprint());
}

TEST(SignalParser, FingerprintDifferentAlerts) {
    auto body_buy = make_entry_json("buy", "bull", 100.0);
    // Change the alert to TP1
    json j = json::parse(body_buy);
    j["alert"] = "TP1";
    auto body_tp = j.dump();

    auto sig_buy = WebhookSignal::from_json(body_buy);
    auto sig_tp  = WebhookSignal::from_json(body_tp);
    ASSERT_TRUE(sig_buy.has_value());
    ASSERT_TRUE(sig_tp.has_value());

    EXPECT_NE(sig_buy->fingerprint(), sig_tp->fingerprint());
}

// ── Priority calculation ──
TEST(SignalParser, PriorityByTimeframe) {
    auto make_with_tf = [](const std::string& tf) {
        json j;
        j["algorithm"]        = "SFX";
        j["ticker"]           = "BINANCE:BTCUSDT.P";
        j["alert"]            = "buy";
        j["signal_direction"] = "bull";
        j["close"]            = 100.0;
        j["timeframe"]        = tf;
        return WebhookSignal::from_json(j.dump());
    };

    EXPECT_EQ(make_with_tf("1")->priority, 0);    // 1m -> 0
    EXPECT_EQ(make_with_tf("5")->priority, 1);    // 5m -> 1
    EXPECT_EQ(make_with_tf("15")->priority, 2);   // 15m -> 2
    EXPECT_EQ(make_with_tf("1h")->priority, 3);   // 1h -> 3
    EXPECT_EQ(make_with_tf("4h")->priority, 3);   // 4h -> 3
    EXPECT_EQ(make_with_tf("1d")->priority, 3);   // 1d -> 3
}

// ── current_rating as string ──
TEST(SignalParser, CurrentRatingAsString) {
    json j;
    j["algorithm"]        = "SFX";
    j["ticker"]           = "BINANCE:BTCUSDT.P";
    j["alert"]            = "buy";
    j["signal_direction"] = "bull";
    j["close"]            = 100.0;
    j["current_rating"]   = "7";

    auto opt = WebhookSignal::from_json(j.dump());
    ASSERT_TRUE(opt.has_value());
    EXPECT_EQ(opt->current_rating, 7);
}

// ── Price field as string ──
TEST(SignalParser, PriceFieldStringFallback) {
    json j;
    j["algorithm"]        = "SFX";
    j["ticker"]           = "BINANCE:BTCUSDT.P";
    j["alert"]            = "buy";
    j["signal_direction"] = "bull";
    j["close"]            = 100.0;
    j["tp1"]              = "115.5";

    auto opt = WebhookSignal::from_json(j.dump());
    ASSERT_TRUE(opt.has_value());
    EXPECT_DOUBLE_EQ(opt->tp1, 115.5);
}

// ── Generic format parsing ──
TEST(SignalParser, GenericFormat) {
    json j;
    j["action"]     = "buy";
    j["symbol"]     = "ETHUSDT";
    j["price"]      = 3500.0;
    j["qty"]        = 0.5;
    j["tp"]         = 3800.0;
    j["sl"]         = 3200.0;
    j["token"]      = "abc123";

    auto opt = WebhookSignal::from_json(j.dump());
    ASSERT_TRUE(opt.has_value());
    EXPECT_EQ(opt->action, "buy");
    EXPECT_EQ(opt->symbol, "ETHUSDT");
    EXPECT_DOUBLE_EQ(opt->price, 3500.0);
    EXPECT_DOUBLE_EQ(opt->size, 0.5);
    EXPECT_DOUBLE_EQ(opt->tp1, 3800.0);
    EXPECT_DOUBLE_EQ(opt->sl, 3200.0);
    EXPECT_EQ(opt->sig_type, SignalType::Entry);
}

// ── is_valid edge cases ──
TEST(SignalParser, IsValidRequiresSymbolAndPrice) {
    WebhookSignal sig;
    sig.sig_type = SignalType::Entry;
    sig.action = "buy";
    // symbol empty, price 0 -> invalid
    EXPECT_FALSE(sig.is_valid());

    sig.symbol = "BTCUSDT";
    sig.price = 100.0;
    EXPECT_TRUE(sig.is_valid());
}

TEST(SignalParser, UnknownSignalTypeIsInvalid) {
    WebhookSignal sig;
    sig.sig_type = SignalType::Unknown;
    sig.symbol = "BTCUSDT";
    sig.price = 100.0;
    sig.action = "buy";
    EXPECT_FALSE(sig.is_valid());
}

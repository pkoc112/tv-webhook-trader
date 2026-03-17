// ============================================================================
// main.cpp -- TradingView Webhook -> Bitget Futures 자동매매 서버
// v3.0 | 2026-03-13 | 고급 리스크 엔진 통합
//
// 변경 이력:
//   v1.0  초기 생성
//   v2.0  trading config, graceful shutdown, 심볼 화이트리스트
//   v3.0  SymbolScorer, FeeAnalyzer, PositionSizer, PortfolioRiskManager 통합
//         StatePersistence, Shadow 모드, TF 필터
// ============================================================================

#include <iostream>
#include <fstream>
#include <csignal>
#include <atomic>
#include <filesystem>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include "core/types.hpp"
#include "core/spsc_queue.hpp"
#include "core/state_persistence.hpp"
#include "webhook/signal_types.hpp"
#include "webhook/http_server.hpp"
#include "exchange/bitget_auth.hpp"
#include "exchange/bitget_rest.hpp"
#include "risk/risk_manager.hpp"
#include "risk/symbol_scorer.hpp"
#include "risk/fee_analyzer.hpp"
#include "risk/position_sizer.hpp"
#include "risk/portfolio_risk.hpp"
#include "core/alert_manager.hpp"
#include "risk/symbol_learner.hpp"
#include "execution/execution_engine.hpp"
#include "dashboard/dashboard_api.hpp"
#include "dashboard/tf_analytics.hpp"

using json = nlohmann::json;

static std::atomic<bool> g_running{true};
static std::atomic<hft::WebhookServer*> g_server{nullptr};

void signal_handler(int sig) {
    spdlog::info("Signal {} received, shutting down...", sig);
    g_running.store(false, std::memory_order_relaxed);
    auto* srv = g_server.load(std::memory_order_relaxed);
    if (srv) srv->stop();
}

void init_logger(const std::string& log_level) {
    auto console_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
        "logs/trading.log", 50 * 1024 * 1024, 5);

    auto logger = std::make_shared<spdlog::logger>("main",
        spdlog::sinks_init_list{console_sink, file_sink});
    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v");

    if (log_level == "debug") logger->set_level(spdlog::level::debug);
    else if (log_level == "warn") logger->set_level(spdlog::level::warn);
    else logger->set_level(spdlog::level::info);

    spdlog::set_default_logger(logger);
}

json load_config(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) throw std::runtime_error("Cannot open config: " + path);
    return json::parse(f);
}

hft::TradingConfig parse_trading_config(const json& config, const json& risk_config) {
    hft::TradingConfig tc;

    if (config.contains("trading")) {
        auto& t = config["trading"];
        tc.default_size      = t.value("default_size", 0.01);
        tc.trade_amount_usdt = t.value("trade_amount_usdt", 10.0);
        tc.default_leverage  = t.value("default_leverage", 10);
        tc.num_workers       = t.value("num_workers", 4);
        tc.order_rate_limit  = t.value("order_rate_limit", 9.0);
        tc.tpsl_rate_limit   = t.value("tpsl_rate_limit", 9.0);
        tc.shadow_mode       = t.value("shadow_mode", false);
        tc.backup_sl_pct     = t.value("backup_sl_pct", 0.03);

        if (t.contains("symbols")) {
            for (auto& s : t["symbols"])
                tc.allowed_symbols.insert(s.get<std::string>());
        }

        if (t.contains("symbol_config")) {
            for (auto& [sym, cfg] : t["symbol_config"].items()) {
                if (cfg.contains("size")) tc.symbol_sizes[sym] = cfg["size"].get<double>();
                if (cfg.contains("leverage")) tc.symbol_leverages[sym] = cfg["leverage"].get<int>();
            }
        }
    }

    // TF 필터 (risk_config에서)
    if (risk_config.contains("tf_filters")) {
        for (auto& [tf, filt] : risk_config["tf_filters"].items()) {
            hft::TfFilter f;
            f.size_multiplier      = filt.value("size_multiplier", 1.0);
            f.min_tier             = filt.value("min_tier", "C");
            f.min_tp_to_cost_ratio = filt.value("min_tp_to_cost_ratio", 1.5);
            f.block_reverse        = filt.value("block_reverse", true);
            tc.tf_filters[tf] = f;
        }
    }

    return tc;
}

int main(int argc, char* argv[]) {
    std::string config_path = "config/config.json";
    std::string risk_config_path = "config/risk_config.json";
    if (argc > 1) config_path = argv[1];
    if (argc > 2) risk_config_path = argv[2];

    try {
        // 설정 로드
        auto config = load_config(config_path);
        auto keys   = load_config(config["api_keys_path"].get<std::string>());

        // 리스크 설정 로드
        json risk_config;
        if (std::filesystem::exists(risk_config_path)) {
            risk_config = load_config(risk_config_path);
            spdlog::info("Risk config loaded: {}", risk_config_path);
        } else {
            spdlog::warn("Risk config not found: {}, using defaults", risk_config_path);
        }

        // 로거 초기화
        std::filesystem::create_directories("logs");
        std::filesystem::create_directories("data");
        init_logger(config.value("log_level", "info"));

        spdlog::info("================================================");
        spdlog::info("  TV Webhook Trader v3.0 (C++ HFT)");
        spdlog::info("  Risk Engine: Symbol Scorer + Fee Analyzer");
        spdlog::info("  + Position Sizer + Portfolio Risk Manager");
        spdlog::info("================================================");

        std::signal(SIGINT, signal_handler);
        std::signal(SIGTERM, signal_handler);

        // -- 1. 시그널 큐 --
        hft::MPSCQueue<hft::WebhookSignal> signal_queue(8192);

        // -- 2. Bitget 인증 --
        hft::BitgetAuth auth(
            keys["api_key"].get<std::string>(),
            keys["api_secret"].get<std::string>(),
            keys["passphrase"].get<std::string>()
        );

        hft::BitgetRestConfig rest_config;
        if (config.contains("exchange")) {
            auto& ex = config["exchange"];
            rest_config.base_host   = ex.value("host", "api.bitget.com");
            rest_config.base_port   = ex.value("port", "443");
            rest_config.timeout_sec = ex.value("timeout_sec", 10);
        }

        // -- 3. 기본 리스크 매니저 --
        hft::RiskLimits risk_limits;
        if (config.contains("risk")) {
            auto& rc = config["risk"];
            risk_limits.max_position_per_symbol = rc.value("max_position_per_symbol", 1.0);
            risk_limits.max_daily_loss          = rc.value("max_daily_loss", 500.0);
            risk_limits.max_order_size          = rc.value("max_order_size", 0.5);
            risk_limits.max_open_orders         = rc.value("max_open_orders", 10);
            risk_limits.circuit_breaker_loss    = rc.value("circuit_breaker_loss", 1000.0);
            risk_limits.max_drawdown_pct        = rc.value("max_drawdown_pct", 5.0);
        }
        hft::RiskManager risk_mgr(risk_limits);

        // -- 4. 고급 리스크 모듈 초기화 --
        hft::SymbolScorer scorer(risk_config, "data");
        hft::FeeAnalyzer fee_analyzer(risk_config);
        hft::PositionSizer position_sizer(risk_config);
        hft::PortfolioRiskManager portfolio_risk(risk_config);
        hft::StatePersistence state_store("data");

        spdlog::info("Risk modules initialized:");
        spdlog::info("  SymbolScorer: min_trades={}", risk_config.value("symbol_scoring", json::object()).value("min_trades", 20));
        spdlog::info("  FeeAnalyzer: taker={:.2f}% maker={:.2f}%",
            risk_config.value("fee_analysis", json::object()).value("taker_fee_pct", 0.06),
            risk_config.value("fee_analysis", json::object()).value("maker_fee_pct", 0.02));
        {
            auto pr_cfg = risk_config.value("portfolio_risk", json::object());
            spdlog::info("  PortfolioRisk: max_concurrent={} max_same_dir={} max_total={}",
                pr_cfg.value("max_concurrent_positions", 15),
                pr_cfg.value("max_same_direction", 10),
                pr_cfg.value("max_positions_total", 50));
        }

        // -- 5. 트레이딩 설정 --
        auto trading_config = parse_trading_config(config, risk_config);

        if (!trading_config.allowed_symbols.empty()) {
            risk_mgr.set_allowed_symbols(trading_config.allowed_symbols);
            spdlog::info("Allowed symbols: {}", trading_config.allowed_symbols.size());
        }

        spdlog::info("Trade amount: {:.2f} USDT | Leverage: {}x | Workers: {} | Shadow: {}",
            trading_config.trade_amount_usdt, trading_config.default_leverage,
            trading_config.num_workers, trading_config.shadow_mode);
        spdlog::info("TF filters: {}", trading_config.tf_filters.size());

        // -- 5.5. 알림 매니저 --
        hft::AlertManager alert_mgr;
        alert_mgr.info("SYSTEM", "Server starting...");

        // -- 5.6. 적응형 학습 엔진 --
        hft::SymbolLearner symbol_learner(risk_config, "data");
        spdlog::info("  SymbolLearner: L1-L4 adaptive learning engine");

        // -- 6. 실행 엔진 (모든 리스크 모듈 연결) --
        hft::EngineConfig engine_cfg{
            .signal_queue    = signal_queue,
            .auth            = auth,
            .rest_config     = rest_config,
            .risk_mgr        = risk_mgr,
            .scorer          = scorer,
            .fee_analyzer    = fee_analyzer,
            .sizer           = position_sizer,
            .portfolio_risk  = portfolio_risk,
            .state_store     = state_store,
            .alerts          = alert_mgr,
            .learner         = symbol_learner,
            .trading_config  = trading_config
        };
        hft::ExecutionEngine exec_engine(engine_cfg);
        exec_engine.start();

        // -- 7. 대시보드 서버 (HTTP) --
        uint16_t dash_port = config.value("dashboard_port", 5000);
        hft::DashboardCallbacks dash_cb{
            .get_stats = [&exec_engine]() { return exec_engine.get_stats(); },
            .get_risk_status = [&exec_engine, &portfolio_risk, &scorer]() {
                auto state = exec_engine.portfolio_state();
                auto check_stats = portfolio_risk.get_check_stats();

                // 학습 진행 상황: 티어별 심볼 수
                auto all_scores = scorer.scores_snapshot();
                nlohmann::json learning;
                std::unordered_map<std::string, int> tier_dist;
                int data_sufficient = 0;
                for (auto& [_, s] : all_scores) {
                    tier_dist[s.tier]++;
                    if (s.data_sufficient) data_sufficient++;
                }
                learning["total_scored"] = all_scores.size();
                learning["data_sufficient"] = data_sufficient;
                learning["tier_distribution"] = tier_dist;
                learning["shadow_mode"] = portfolio_risk.is_shadow_mode();
                learning["live_min_tier"] = portfolio_risk.get_live_min_tier();

                // Live 모드 진입 가능 심볼 수
                std::string min_tier = portfolio_risk.get_live_min_tier();
                int live_ready = 0;
                for (auto& [_, s] : all_scores) {
                    if (s.data_sufficient) {
                        // B 이상이면 live ready
                        if (min_tier == "S" && s.tier == "S") live_ready++;
                        else if (min_tier == "A" && (s.tier == "S" || s.tier == "A")) live_ready++;
                        else if (min_tier == "B" && (s.tier == "S" || s.tier == "A" || s.tier == "B")) live_ready++;
                        else if (min_tier == "C" && s.tier != "D" && s.tier != "X") live_ready++;
                    }
                }
                learning["live_ready_symbols"] = live_ready;

                return nlohmann::json{
                    {"portfolio", state},
                    {"check_stats", check_stats},
                    {"risk_skips", exec_engine.risk_skip_count()},
                    {"learning", learning}
                };
            },
            .get_symbol_scores = [&scorer]() { return scorer.get_all_scores_json(); },
            .get_positions = [&exec_engine]() {
                auto positions = exec_engine.positions_snapshot();
                auto arr = nlohmann::json::array();
                for (auto& [key, p] : positions) {
                    arr.push_back(nlohmann::json{
                        {"key", key}, {"symbol", p.symbol}, {"timeframe", p.timeframe},
                        {"side", p.side}, {"entry_price", p.entry_price},
                        {"quantity", p.quantity}, {"leverage", p.leverage},
                        {"tier", p.tier}, {"strategy", p.strategy}
                    });
                }
                return arr;
            },
            .get_alerts = [&alert_mgr]() { return alert_mgr.get_alerts_json(100); },
            .get_learner = [&symbol_learner]() { return symbol_learner.get_learner_json(); },
            .get_learner_summary = [&symbol_learner]() { return symbol_learner.get_summary_json(); },
            .get_tf_stats = [&exec_engine]() {
                auto trades = exec_engine.trades_snapshot();
                return hft::TfAnalytics::analyze(trades);
            },
            .get_strategy_stats = [&exec_engine]() {
                return exec_engine.get_strategy_stats();
            }
        };

        std::string static_dir = config.value("static_dir", "static");
        std::filesystem::create_directories(static_dir);
        std::string dash_token = config.value("dashboard_token", "");
        std::string dash_cors_origin = config.value("dashboard_cors_origin", "");
        hft::DashboardServer dashboard(dash_port, static_dir, dash_cb, dash_token, dash_cors_origin);
        dashboard.start();
        spdlog::info("Dashboard on port {}", dash_port);

        // -- 8. Webhook 서버 --
        hft::WebhookConfig wh_config;
        if (config.contains("webhook")) {
            auto& wc = config["webhook"];
            wh_config.bind_address   = wc.value("bind_address", "0.0.0.0");
            wh_config.port           = static_cast<uint16_t>(wc.value("port", 8443));
            wh_config.ssl_cert_path  = wc.value("ssl_cert", "");
            wh_config.ssl_key_path   = wc.value("ssl_key", "");
            wh_config.auth_token     = wc.value("auth_token", "");

            if (wc.contains("allowed_ips")) {
                for (auto& ip : wc["allowed_ips"])
                    wh_config.allowed_ips.insert(ip.get<std::string>());
            }
        }

        if (wh_config.auth_token.empty() || wh_config.auth_token == "CHANGE_ME") {
            if (!trading_config.shadow_mode) {
                spdlog::error("FATAL: Webhook auth token not configured. Set 'auth_token' in config.json");
                return 1;
            }
            spdlog::warn("WARNING: Using default/empty auth token (shadow mode)");
        }

        auto on_signal = [&signal_queue](hft::WebhookSignal&& sig) {
            auto action = sig.action;
            auto symbol = sig.symbol;
            if (!signal_queue.try_push(std::move(sig))) {
                spdlog::error("[QUEUE] Full! Dropping: {} {}", action, symbol);
            }
        };

        hft::WebhookServer server(wh_config, on_signal);
        g_server.store(&server, std::memory_order_relaxed);

        spdlog::info("Webhook server on port {}", wh_config.port);
        spdlog::info("================================================");

        server.run(config.value("server_threads", 8));

        // 종료
        g_server.store(nullptr, std::memory_order_relaxed);
        dashboard.stop();
        exec_engine.stop();
        symbol_learner.save();
        spdlog::info("Shutdown. Orders: {} | Risk skips: {}",
            exec_engine.orders_executed(), exec_engine.risk_skip_count());

    } catch (const std::exception& e) {
        spdlog::error("Fatal: {}", e.what());
        return 1;
    }

    return 0;
}

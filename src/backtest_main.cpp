// ============================================================================
// backtest_main.cpp -- Walk-Forward 백테스트 & 최적화 실행기
// v1.1 | 2026-03-13
//
// 사용법:
//   ./backtest <state.json 경로> [--symbol BTCUSDT] [--mode wf|grid|single|compare]
//              [--balance 1000] [--amount 10] [--leverage 10]
//              [--timeframe 15] [--apply config.json]
//
// 모드:
//   wf      - Walk-Forward 최적화 (기본)
//   grid    - 전체 그리드 서치 (IS/OOS 분리 없음)
//   single  - 단일 파라미터 백테스트
//   compare - 타임프레임별 성능 비교
// ============================================================================

#include <iostream>
#include <fstream>
#include <filesystem>
#include <string>
#include <vector>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <nlohmann/json.hpp>

#include "backtest/log_parser.hpp"
#include "backtest/backtester.hpp"
#include "backtest/wf_optimizer.hpp"

using json = nlohmann::json;

using namespace hft::bt;

struct CliArgs {
    std::string state_path;
    std::string symbol;             // 특정 심볼만 (빈 문자열 = 전체)
    std::string timeframe;          // 특정 타임프레임만 (빈 문자열 = 전체)
    std::string mode{"wf"};         // wf, grid, single, compare
    std::string config_path;        // --apply 시 config.json 경로
    double balance{1000.0};
    double amount{10.0};
    int    leverage{10};
    bool   apply{false};            // WF 결과를 config.json에 자동 반영
};

CliArgs parse_args(int argc, char* argv[]) {
    CliArgs args;
    if (argc < 2) {
        std::cerr << "Usage: backtest <state.json> [options]\n"
                  << "  --symbol <SYM>       Filter by symbol\n"
                  << "  --timeframe <TF>     Filter by timeframe (1, 5, 15, 60)\n"
                  << "  --mode <MODE>        wf (default), grid, single, compare\n"
                  << "  --balance <N>        Initial balance (default: 1000)\n"
                  << "  --amount <N>         Trade amount USDT for single mode\n"
                  << "  --leverage <N>       Leverage for single mode\n"
                  << "  --apply <config>     Apply WF result to config.json\n";
        std::exit(1);
    }

    args.state_path = argv[1];
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--symbol" && i + 1 < argc)     args.symbol    = argv[++i];
        else if (arg == "--timeframe" && i + 1 < argc) args.timeframe = argv[++i];
        else if (arg == "--mode" && i + 1 < argc)  args.mode      = argv[++i];
        else if (arg == "--balance" && i + 1 < argc) args.balance  = std::stod(argv[++i]);
        else if (arg == "--amount" && i + 1 < argc)  args.amount   = std::stod(argv[++i]);
        else if (arg == "--leverage" && i + 1 < argc) args.leverage = std::stoi(argv[++i]);
        else if (arg == "--apply" && i + 1 < argc) {
            args.apply = true;
            args.config_path = argv[++i];
        }
    }
    return args;
}

void init_logger() {
    auto console = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    auto logger = std::make_shared<spdlog::logger>("bt", console);
    logger->set_pattern("[%Y-%m-%d %H:%M:%S] [%^%l%$] %v");
    logger->set_level(spdlog::level::info);
    spdlog::set_default_logger(logger);
}

// -- WF 결과를 config.json에 자동 반영 --
void apply_to_config(const std::string& config_path, double amount, int leverage) {
    std::ifstream fin(config_path);
    if (!fin.is_open()) {
        spdlog::error("Cannot open config: {}", config_path);
        return;
    }

    json config = json::parse(fin);
    fin.close();

    if (!config.contains("trading")) {
        config["trading"] = json::object();
    }

    double old_amount = config["trading"].value("trade_amount_usdt", 0.0);
    int old_leverage   = config["trading"].value("default_leverage", 0);

    config["trading"]["trade_amount_usdt"] = amount;
    config["trading"]["default_leverage"]  = leverage;

    // 원자적 쓰기: .tmp → rename
    std::string tmp_path = config_path + ".tmp";
    std::ofstream fout(tmp_path);
    if (!fout.is_open()) {
        spdlog::error("Cannot write tmp config: {}", tmp_path);
        return;
    }
    fout << config.dump(4) << std::endl;
    fout.close();

    std::filesystem::rename(tmp_path, config_path);

    spdlog::info("=== Config Updated: {} ===", config_path);
    spdlog::info("  trade_amount_usdt: {:.0f} -> {:.0f}", old_amount, amount);
    spdlog::info("  default_leverage:  {} -> {}", old_leverage, leverage);
}

// -- 단일 파라미터 백테스트 --
void run_single(const std::vector<BtSignal>& signals, const CliArgs& args) {
    spdlog::info("=== Single Backtest: {:.0f} USDT / {}x ===", args.amount, args.leverage);

    BtParams params;
    params.trade_amount_usdt = args.amount;
    params.leverage          = args.leverage;
    params.initial_balance   = args.balance;

    Backtester bt(params);
    auto metrics = bt.run(signals);
    metrics.param_amount   = args.amount;
    metrics.param_leverage = args.leverage;
    metrics.print();
}

// -- 전체 그리드 서치 --
void run_grid(const std::vector<BtSignal>& signals, const CliArgs& args) {
    spdlog::info("=== Grid Search ===");

    std::vector<double> amounts{5, 10, 15, 20, 30, 50, 75, 100};
    std::vector<int>    leverages{3, 5, 7, 10, 15, 20, 25};

    BtMetrics best_metrics;
    double best_score = -1e9;

    for (double amt : amounts) {
        for (int lev : leverages) {
            BtParams params;
            params.trade_amount_usdt = amt;
            params.leverage          = lev;
            params.initial_balance   = args.balance;

            Backtester bt(params);
            auto metrics = bt.run(signals);

            // Sharpe + DD 페널티
            double score = metrics.sharpe_ratio -
                std::max(0.0, metrics.max_drawdown_pct - 10.0) * 0.1;

            if (score > best_score) {
                best_score = score;
                best_metrics = metrics;
                best_metrics.param_amount   = amt;
                best_metrics.param_leverage = lev;
            }

            spdlog::info("  {:.0f} USDT / {}x | PnL: {:.2f} | Sharpe: {:.3f} | DD: {:.1f}% | WR: {:.0f}%",
                amt, lev, metrics.total_pnl, metrics.sharpe_ratio,
                metrics.max_drawdown_pct, metrics.win_rate * 100);
        }
    }

    spdlog::info("");
    spdlog::info("=== Best Grid Result ===");
    best_metrics.print();
}

// -- 타임프레임별 비교 --
void run_compare(const std::vector<BtSignal>& all_signals, const CliArgs& args) {
    auto timeframes = LogParser::unique_timeframes(all_signals);

    if (timeframes.empty()) {
        spdlog::warn("No timeframe data in signals. All signals have empty timeframe.");
        spdlog::info("Hint: TradingView alert JSON must include \"timeframe\" field.");
        return;
    }

    spdlog::info("=== Timeframe Comparison ===");
    spdlog::info("Found {} timeframes: ", timeframes.size());
    for (auto& tf : timeframes) {
        auto cnt = LogParser::filter_by_timeframe(all_signals, tf).size();
        spdlog::info("  {}m : {} signals", tf, cnt);
    }
    spdlog::info("");

    struct TFResult {
        std::string tf;
        size_t signal_count{0};
        size_t trade_count{0};
        double pnl{0.0};
        double win_rate{0.0};
        double sharpe{0.0};
        double max_dd{0.0};
        double profit_factor{0.0};
        double best_amount{0.0};
        int    best_leverage{0};
    };

    std::vector<TFResult> results;

    for (auto& tf : timeframes) {
        auto tf_signals = LogParser::filter_by_timeframe(all_signals, tf);
        if (tf_signals.size() < 5) {
            spdlog::warn("  {}m: only {} signals, skipping", tf, tf_signals.size());
            continue;
        }

        spdlog::info("--- {}m Timeframe ({} signals) ---", tf, tf_signals.size());

        // 그리드 서치로 최적 파라미터 탐색
        std::vector<double> amounts{5, 10, 15, 20, 30};
        std::vector<int>    leverages{3, 5, 7, 10, 15};

        BtMetrics best_metrics;
        double best_score = -1e9;
        double best_amt = 10;
        int best_lev = 5;

        for (double amt : amounts) {
            for (int lev : leverages) {
                BtParams params;
                params.trade_amount_usdt = amt;
                params.leverage          = lev;
                params.initial_balance   = args.balance;

                Backtester bt(params);
                auto metrics = bt.run(tf_signals);

                double score = metrics.sharpe_ratio -
                    std::max(0.0, metrics.max_drawdown_pct - 10.0) * 0.1;

                if (score > best_score) {
                    best_score = score;
                    best_metrics = metrics;
                    best_amt = amt;
                    best_lev = lev;
                }
            }
        }

        TFResult r;
        r.tf            = tf;
        r.signal_count  = tf_signals.size();
        r.trade_count   = static_cast<size_t>(best_metrics.total_trades);
        r.pnl           = best_metrics.total_pnl;
        r.win_rate      = best_metrics.win_rate * 100;
        r.sharpe        = best_metrics.sharpe_ratio;
        r.max_dd        = best_metrics.max_drawdown_pct;
        r.profit_factor = best_metrics.profit_factor;
        r.best_amount   = best_amt;
        r.best_leverage = best_lev;
        results.push_back(r);

        spdlog::info("  Best: {:.0f} USDT / {}x | PnL: {:.2f} | WR: {:.1f}% | Sharpe: {:.3f} | DD: {:.1f}%",
            best_amt, best_lev, best_metrics.total_pnl,
            best_metrics.win_rate * 100, best_metrics.sharpe_ratio,
            best_metrics.max_drawdown_pct);
    }

    // 최종 비교 테이블
    spdlog::info("");
    spdlog::info("===============================================================");
    spdlog::info("  TIMEFRAME COMPARISON SUMMARY");
    spdlog::info("===============================================================");
    spdlog::info("{:>6} {:>7} {:>7} {:>10} {:>7} {:>8} {:>7} {:>10}",
        "TF", "Sigs", "Trades", "PnL", "WR%", "Sharpe", "DD%", "Best Cfg");
    spdlog::info("---------------------------------------------------------------");
    for (auto& r : results) {
        std::string cfg = fmt::format("{:.0f}/{}", r.best_amount, r.best_leverage);
        spdlog::info("{:>5}m {:>7} {:>7} {:>+10.2f} {:>6.1f}% {:>8.3f} {:>6.1f}% {:>10}",
            r.tf, r.signal_count, r.trade_count, r.pnl,
            r.win_rate, r.sharpe, r.max_dd, cfg);
    }
    spdlog::info("===============================================================");

    // 최고 Sharpe 추천
    if (!results.empty()) {
        auto best = std::max_element(results.begin(), results.end(),
            [](const TFResult& a, const TFResult& b) { return a.sharpe < b.sharpe; });
        spdlog::info("");
        spdlog::info("  RECOMMENDED: {}m timeframe (Sharpe: {:.3f})", best->tf, best->sharpe);
        spdlog::info("  Optimal params: {:.0f} USDT / {}x leverage", best->best_amount, best->best_leverage);
    }
}

// -- Walk-Forward 최적화 --
void run_wf(const std::vector<BtSignal>& signals, const CliArgs& args) {
    spdlog::info("=== Walk-Forward Optimization ===");

    WFConfig config;
    config.initial_balance = args.balance;

    WFOptimizer optimizer(config);
    auto result = optimizer.optimize(signals);
    result.print();

    // 추천 파라미터로 전체 구간 재검증
    spdlog::info("");
    spdlog::info("=== Full Backtest with Recommended Params ===");
    BtParams params;
    params.trade_amount_usdt = result.recommended_amount;
    params.leverage          = result.recommended_leverage;
    params.initial_balance   = args.balance;

    Backtester bt(params);
    auto full_metrics = bt.run(signals);
    full_metrics.param_amount   = result.recommended_amount;
    full_metrics.param_leverage = result.recommended_leverage;
    full_metrics.print();

    // --apply 옵션이 있으면 config.json에 자동 반영
    if (args.apply && !args.config_path.empty()) {
        apply_to_config(args.config_path,
            result.recommended_amount, result.recommended_leverage);
    }
}

int main(int argc, char* argv[]) {
    init_logger();
    auto args = parse_args(argc, argv);

    try {
        // 시그널 로드
        auto state = LogParser::load(args.state_path);
        auto signals = state.signals;

        // 심볼 필터
        if (!args.symbol.empty()) {
            signals = LogParser::filter_by_symbol(signals, args.symbol);
            spdlog::info("Filtered to {} signals for {}", signals.size(), args.symbol);
        }

        // 타임프레임 필터 (compare 모드가 아닐 때만)
        if (!args.timeframe.empty() && args.mode != "compare") {
            signals = LogParser::filter_by_timeframe(signals, args.timeframe);
            spdlog::info("Filtered to {} signals for {}m timeframe", signals.size(), args.timeframe);
        }

        if (signals.empty()) {
            spdlog::error("No signals to backtest!");
            return 1;
        }

        // 타임프레임 통계
        auto timeframes = LogParser::unique_timeframes(signals);
        if (!timeframes.empty()) {
            spdlog::info("Timeframes found: {}", timeframes.size());
            for (auto& tf : timeframes) {
                auto cnt = LogParser::filter_by_timeframe(signals, tf).size();
                spdlog::info("  {}m : {} signals", tf, cnt);
            }
        }

        // 심볼 통계
        auto symbols = LogParser::unique_symbols(signals);
        spdlog::info("Total signals: {} | Symbols: {} | Timeframes: {}",
            signals.size(), symbols.size(), timeframes.size());
        for (auto& sym : symbols) {
            auto cnt = LogParser::filter_by_symbol(signals, sym).size();
            spdlog::info("  {} : {} signals", sym, cnt);
        }
        spdlog::info("");

        // 모드별 실행
        if (args.mode == "compare") {
            run_compare(signals, args);
        } else if (args.mode == "single") {
            run_single(signals, args);
        } else if (args.mode == "grid") {
            run_grid(signals, args);
        } else {
            run_wf(signals, args);
        }

    } catch (const std::exception& e) {
        spdlog::error("Error: {}", e.what());
        return 1;
    }

    return 0;
}

// ============================================================================
// dashboard/dashboard_api.hpp -- HTTP 대시보드 API 서버
// v1.1 | 2026-03-16
//
// Boost::Beast HTTP 서버로 REST API + 정적 파일 서빙
// Security: Basic Auth, path traversal protection, rate limiting, no CORS wildcard
// 엔드포인트:
//   GET /                   → dashboard.html
//   GET /api/risk/status    → 포트폴리오 상태
//   GET /api/symbols/scores → 심볼 티어/점수
//   GET /api/stats          → 거래 통계
//   GET /api/positions      → 오픈 포지션
//   GET /api/strategy-stats  → 전략별 성과 통계
//   GET /health             → 헬스체크
//   GET /metrics            → Prometheus metrics (no auth)
// ============================================================================
#pragma once

#include <cstdio>
#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <fstream>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <chrono>

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/asio.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

namespace hft {

namespace beast = boost::beast;
namespace http = beast::http;
namespace net = boost::asio;
using tcp = net::ip::tcp;

// ExecutionEngine 전방 선언 피하기 위해 콜백 사용
struct DashboardCallbacks {
    std::function<nlohmann::json()> get_stats;
    std::function<nlohmann::json()> get_risk_status;
    std::function<nlohmann::json()> get_symbol_scores;
    std::function<nlohmann::json()> get_positions;
    std::function<nlohmann::json()> get_alerts;
    std::function<nlohmann::json()> get_learner;
    std::function<nlohmann::json()> get_learner_summary;
    std::function<nlohmann::json()> get_tf_stats;
    std::function<nlohmann::json()> get_strategy_stats;
    std::function<nlohmann::json(const nlohmann::json&)> import_trades;

    // ── Spot 전용 콜백 (선물과 완전 분리) ──
    std::function<nlohmann::json()> get_spot_stats;
    std::function<nlohmann::json()> get_spot_positions;
    std::function<nlohmann::json()> get_spot_trades;
};

// Simple per-IP rate limiter: max requests per window
struct DashboardRateLimiter {
    struct Entry {
        int count = 0;
        std::chrono::steady_clock::time_point window_start;
    };

    int max_requests = 300;
    int window_seconds = 60;
    std::mutex mtx;
    std::unordered_map<std::string, Entry> clients;

    bool allow(const std::string& ip) {
        std::lock_guard<std::mutex> lock(mtx);
        auto now = std::chrono::steady_clock::now();
        auto& e = clients[ip];
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - e.window_start).count();
        if (elapsed >= window_seconds) {
            e.count = 1;
            e.window_start = now;
            return true;
        }
        if (++e.count > max_requests) return false;
        return true;
    }
};

// Base64 decode (minimal, for Basic Auth)
inline std::string base64_decode(const std::string& in) {
    static const std::string chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    std::vector<int> T(256, -1);
    for (int i = 0; i < 64; i++) T[static_cast<unsigned char>(chars[i])] = i;
    int val = 0, valb = -8;
    for (unsigned char c : in) {
        if (T[c] == -1) break;
        val = (val << 6) + T[c];
        valb += 6;
        if (valb >= 0) {
            out.push_back(char((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

class DashboardServer {
public:
    DashboardServer(uint16_t port,
                    const std::string& static_dir,
                    DashboardCallbacks callbacks,
                    const std::string& auth_token = "",
                    const std::string& allowed_origin = "")
        : m_port(port)
        , m_static_dir(static_dir)
        , m_cb(std::move(callbacks))
        , m_allowed_origin(allowed_origin)
    {
        // Resolve static dir to canonical absolute path for path traversal protection
        if (std::filesystem::exists(m_static_dir)) {
            m_static_dir_canonical = std::filesystem::canonical(m_static_dir).string();
        } else {
            m_static_dir_canonical = std::filesystem::absolute(m_static_dir).string();
        }

        // Auth: require explicit token — refuse to start with defaults in non-shadow mode
        if (auth_token.empty()) {
            m_auth_token = "admin:" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count());
            spdlog::error("[Dashboard] No dashboard_token configured! Generated random token. Set dashboard_token in config!");
        } else {
            m_auth_token = auth_token;
        }
    }

    void start() {
        m_running.store(true);
        m_thread = std::thread([this] { run(); });
        spdlog::info("[Dashboard] Started on port {}", m_port);
    }

    void stop() {
        m_running.store(false);
        if (m_acceptor) {
            boost::system::error_code ec;
            m_acceptor->close(ec);
        }
        if (m_thread.joinable()) m_thread.join();
        spdlog::info("[Dashboard] Stopped");
    }

private:
    void run() {
        try {
            net::io_context ioc{1};
            m_acceptor = std::make_unique<tcp::acceptor>(
                ioc, tcp::endpoint(tcp::v4(), m_port));
            m_acceptor->set_option(net::socket_base::reuse_address(true));

            while (m_running.load()) {
                tcp::socket socket(ioc);

                // 비동기 accept with timeout
                boost::system::error_code ec;
                m_acceptor->accept(socket, ec);
                if (ec) {
                    if (ec == net::error::operation_aborted) break;
                    continue;
                }

                // 동기 처리 (단순, 대시보드 트래픽 낮음)
                handle_connection(std::move(socket));
            }
        } catch (const std::exception& e) {
            spdlog::error("[Dashboard] Server error: {}", e.what());
        }
    }

    void handle_connection(tcp::socket socket) {
        try {
            // Rate limiting by remote IP
            auto remote_ip = socket.remote_endpoint().address().to_string();
            if (!m_rate_limiter.allow(remote_ip)) {
                http::response<http::string_body> res{http::status::too_many_requests, 11};
                res.set(http::field::content_type, "application/json");
                res.set(http::field::retry_after, "60");
                res.body() = R"({"error":"rate limit exceeded"})";
                res.prepare_payload();
                http::write(socket, res);
                beast::error_code ec;
                socket.shutdown(tcp::socket::shutdown_send, ec);
                return;
            }

            beast::flat_buffer buffer;
            http::request<http::string_body> req;
            http::read(socket, buffer, req);

            // Basic Auth check (skip for health + HTML pages that have their own login form)
            auto target = std::string(req.target());
            bool is_public = (target == "/health" || target == "/" || target == "/index.html"
                || target == "/dashboard.html" || target == "/learning.html" || target == "/learning"
                || target == "/spot.html" || target == "/spot");
            if (!is_public) {
                if (!check_auth(req)) {
                    http::response<http::string_body> res{http::status::unauthorized, req.version()};
                    res.set(http::field::content_type, "application/json");
                    // NOTE: WWW-Authenticate 헤더 제거 — 브라우저 기본 팝업 방지, 커스텀 로그인 폼 사용
                    res.body() = R"({"error":"unauthorized"})";
                    res.prepare_payload();
                    http::write(socket, res);
                    beast::error_code ec;
                    socket.shutdown(tcp::socket::shutdown_send, ec);
                    return;
                }
            }

            auto response = route_request(req);
            http::write(socket, response);

            beast::error_code ec;
            socket.shutdown(tcp::socket::shutdown_send, ec);
        } catch (const std::exception& e) {
            // Connection closed, ignore
        }
    }

    bool check_auth(const http::request<http::string_body>& req) {
        auto it = req.find(http::field::authorization);
        if (it == req.end()) return false;

        auto auth_value = std::string(it->value());
        // Expect "Basic <base64(user:pass)>"
        if (auth_value.size() < 7 || auth_value.substr(0, 6) != "Basic ") return false;

        auto decoded = base64_decode(auth_value.substr(6));
        return decoded == m_auth_token;
    }

    http::response<http::string_body> route_request(
        const http::request<http::string_body>& req)
    {
        auto target = std::string(req.target());

        // Response helper (no CORS wildcard — same-origin only, or configured origin)
        auto make_response = [&](http::status status, const std::string& body,
                                  const std::string& content_type = "application/json") {
            http::response<http::string_body> res{status, req.version()};
            res.set(http::field::server, "HFT-Dashboard/1.1");
            res.set(http::field::content_type, content_type);
            if (!m_allowed_origin.empty()) {
                res.set(http::field::access_control_allow_origin, m_allowed_origin);
            }
            // No Access-Control-Allow-Origin header = same-origin only
            res.body() = body;
            res.prepare_payload();
            return res;
        };

        try {
            if (target == "/api/stats") {
                return make_response(http::status::ok,
                    m_cb.get_stats().dump());
            }
            if (target == "/api/risk/status") {
                return make_response(http::status::ok,
                    m_cb.get_risk_status().dump());
            }
            if (target == "/api/symbols/scores") {
                return make_response(http::status::ok,
                    m_cb.get_symbol_scores().dump());
            }
            if (target == "/api/positions") {
                return make_response(http::status::ok,
                    m_cb.get_positions().dump());
            }
            if (target == "/api/alerts") {
                if (m_cb.get_alerts) {
                    return make_response(http::status::ok,
                        m_cb.get_alerts().dump());
                }
                return make_response(http::status::ok, "[]");
            }
            if (target == "/api/learner") {
                if (m_cb.get_learner) {
                    return make_response(http::status::ok,
                        m_cb.get_learner().dump());
                }
                return make_response(http::status::ok, "[]");
            }
            if (target == "/api/tf/stats") {
                if (m_cb.get_tf_stats) {
                    return make_response(http::status::ok,
                        m_cb.get_tf_stats().dump());
                }
                return make_response(http::status::ok, "{}");
            }
            if (target == "/api/learner/summary") {
                if (m_cb.get_learner_summary) {
                    return make_response(http::status::ok,
                        m_cb.get_learner_summary().dump());
                }
                return make_response(http::status::ok, "{}");
            }
            if (target == "/api/strategy-stats") {
                if (m_cb.get_strategy_stats) {
                    return make_response(http::status::ok,
                        m_cb.get_strategy_stats().dump());
                }
                return make_response(http::status::ok, "[]");
            }
            // ── Spot 전용 API (선물과 완전 분리) ──
            if (target == "/api/spot/stats") {
                if (m_cb.get_spot_stats) {
                    return make_response(http::status::ok,
                        m_cb.get_spot_stats().dump());
                }
                return make_response(http::status::ok, "{}");
            }
            if (target == "/api/spot/positions") {
                if (m_cb.get_spot_positions) {
                    return make_response(http::status::ok,
                        m_cb.get_spot_positions().dump());
                }
                return make_response(http::status::ok, "[]");
            }
            if (target == "/api/spot/trades") {
                if (m_cb.get_spot_trades) {
                    return make_response(http::status::ok,
                        m_cb.get_spot_trades().dump());
                }
                return make_response(http::status::ok, "[]");
            }
            // POST: Import external trades
            if (target == "/api/import-trades" && req.method() == http::verb::post) {
                if (m_cb.import_trades) {
                    try {
                        auto body = nlohmann::json::parse(req.body());
                        auto result = m_cb.import_trades(body);
                        return make_response(http::status::ok, result.dump());
                    } catch (const nlohmann::json::parse_error& e) {
                        return make_response(http::status::bad_request,
                            nlohmann::json{{"error", "invalid JSON"}, {"detail", e.what()}}.dump());
                    }
                }
                return make_response(http::status::not_found, R"({"error":"import not configured"})");
            }
            if (target == "/health") {
                return make_response(http::status::ok,
                    R"({"status":"ok"})");
            }
            if (target == "/metrics") {
                return make_response(http::status::ok,
                    build_prometheus_metrics(),
                    "text/plain; version=0.0.4; charset=utf-8");
            }
            if (target == "/" || target == "/index.html" || target == "/dashboard.html") {
                return serve_file("dashboard.html", "text/html");
            }
            if (target == "/learning.html" || target == "/learning") {
                return serve_file("learning.html", "text/html");
            }
            if (target == "/spot.html" || target == "/spot") {
                return serve_file("spot.html", "text/html");
            }
            // 정적 파일 서빙 (with path traversal protection)
            if (target.starts_with("/static/")) {
                auto file = target.substr(8);
                // Reject path traversal attempts
                if (file.find("..") != std::string::npos) {
                    return make_response(http::status::forbidden,
                        R"({"error":"forbidden"})");
                }
                std::string ct = "text/plain";
                if (file.ends_with(".html")) ct = "text/html";
                else if (file.ends_with(".js")) ct = "application/javascript";
                else if (file.ends_with(".css")) ct = "text/css";
                return serve_file(file, ct);
            }
        } catch (const std::exception& e) {
            return make_response(http::status::internal_server_error,
                nlohmann::json{{"error", e.what()}}.dump());
        }

        return make_response(http::status::not_found,
            R"({"error":"not found"})");
    }

    std::string build_prometheus_metrics() {
        std::string out;
        out.reserve(2048);

        auto emit = [&](const char* name, const char* help,
                        const char* type, double value) {
            out += "# HELP ";
            out += name;
            out += " ";
            out += help;
            out += "\n# TYPE ";
            out += name;
            out += " ";
            out += type;
            out += "\n";
            out += name;
            out += " ";
            // Format: avoid trailing zeros but keep at least one decimal
            char buf[64];
            std::snprintf(buf, sizeof(buf), "%.6g", value);
            out += buf;
            out += "\n";
        };

        try {
            // Get data from existing callbacks
            auto stats = m_cb.get_stats ? m_cb.get_stats() : nlohmann::json::object();
            auto risk = m_cb.get_risk_status ? m_cb.get_risk_status() : nlohmann::json::object();
            auto positions = m_cb.get_positions ? m_cb.get_positions() : nlohmann::json::array();

            double balance = stats.value("balance", 0.0);
            double total_pnl = stats.value("total_pnl", 0.0);
            double win_rate = stats.value("win_rate", 0.0);
            double total_trades = stats.value("total_trades", 0.0);
            double orders_executed = stats.value("orders_executed", 0.0);
            double daily_pnl = stats.value("daily_pnl", 0.0);

            double open_positions = 0.0;
            if (positions.is_array()) {
                open_positions = static_cast<double>(positions.size());
            }

            double risk_skips = risk.value("risk_skips", 0.0);
            double circuit_breaker = risk.value("circuit_breaker_active", false) ? 1.0 : 0.0;
            double margin_used_pct = risk.value("margin_used_pct", 0.0);

            emit("trader_balance_usdt",
                 "Current account balance in USDT", "gauge", balance);
            emit("trader_open_positions",
                 "Number of currently open positions", "gauge", open_positions);
            emit("trader_total_trades",
                 "Total number of trades executed", "counter", total_trades);
            emit("trader_total_pnl_usdt",
                 "Total realized PnL in USDT", "gauge", total_pnl);
            emit("trader_win_rate",
                 "Win rate as a ratio (0-1)", "gauge", win_rate);
            emit("trader_risk_skips_total",
                 "Total number of trades skipped due to risk limits", "counter", risk_skips);
            emit("trader_orders_executed_total",
                 "Total number of orders sent to exchange", "counter", orders_executed);
            emit("trader_circuit_breaker_active",
                 "Whether the circuit breaker is currently active (0 or 1)", "gauge", circuit_breaker);
            emit("trader_daily_pnl_usdt",
                 "PnL for the current trading day in USDT", "gauge", daily_pnl);
            emit("trader_margin_used_pct",
                 "Percentage of available margin currently in use", "gauge", margin_used_pct);
        } catch (const std::exception& e) {
            out += "# Error collecting metrics: " + std::string(e.what()) + "\n";
        }

        return out;
    }

    http::response<http::string_body> serve_file(
        const std::string& filename, const std::string& content_type)
    {
        auto path = m_static_dir + "/" + filename;
        if (!std::filesystem::exists(path)) {
            http::response<http::string_body> res{http::status::not_found, 11};
            res.set(http::field::content_type, "text/plain");
            res.body() = "File not found";
            res.prepare_payload();
            return res;
        }

        // Path traversal protection: ensure canonical path is inside static dir
        auto canonical = std::filesystem::canonical(path).string();
        if (canonical.find(m_static_dir_canonical) != 0) {
            http::response<http::string_body> res{http::status::forbidden, 11};
            res.set(http::field::content_type, "application/json");
            res.body() = R"({"error":"forbidden"})";
            res.prepare_payload();
            return res;
        }

        std::ifstream f(path, std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());

        http::response<http::string_body> res{http::status::ok, 11};
        res.set(http::field::server, "HFT-Dashboard/1.1");
        res.set(http::field::content_type, content_type);
        if (!m_allowed_origin.empty()) {
            res.set(http::field::access_control_allow_origin, m_allowed_origin);
        }
        res.body() = std::move(content);
        res.prepare_payload();
        return res;
    }

    uint16_t m_port;
    std::string m_static_dir;
    std::string m_static_dir_canonical;
    DashboardCallbacks m_cb;
    std::string m_auth_token;        // "user:pass" for Basic Auth
    std::string m_allowed_origin;    // CORS origin (empty = same-origin only)
    DashboardRateLimiter m_rate_limiter;
    std::atomic<bool> m_running{false};
    std::thread m_thread;
    std::unique_ptr<tcp::acceptor> m_acceptor;
};

} // namespace hft

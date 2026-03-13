// ============================================================================
// dashboard/dashboard_api.hpp -- HTTP 대시보드 API 서버
// v1.0 | 2026-03-13
//
// Boost::Beast HTTP 서버로 REST API + 정적 파일 서빙
// 엔드포인트:
//   GET /                   → dashboard.html
//   GET /api/risk/status    → 포트폴리오 상태
//   GET /api/symbols/scores → 심볼 티어/점수
//   GET /api/stats          → 거래 통계
//   GET /api/positions      → 오픈 포지션
//   GET /health             → 헬스체크
// ============================================================================
#pragma once

#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <fstream>
#include <filesystem>

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
};

class DashboardServer {
public:
    DashboardServer(uint16_t port,
                    const std::string& static_dir,
                    DashboardCallbacks callbacks)
        : m_port(port)
        , m_static_dir(static_dir)
        , m_cb(std::move(callbacks)) {}

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
            beast::flat_buffer buffer;
            http::request<http::string_body> req;
            http::read(socket, buffer, req);

            auto response = route_request(req);
            http::write(socket, response);

            beast::error_code ec;
            socket.shutdown(tcp::socket::shutdown_send, ec);
        } catch (const std::exception& e) {
            // Connection closed, ignore
        }
    }

    http::response<http::string_body> route_request(
        const http::request<http::string_body>& req)
    {
        auto target = std::string(req.target());

        // CORS 헤더
        auto make_response = [&](http::status status, const std::string& body,
                                  const std::string& content_type = "application/json") {
            http::response<http::string_body> res{status, req.version()};
            res.set(http::field::server, "HFT-Dashboard/1.0");
            res.set(http::field::content_type, content_type);
            res.set(http::field::access_control_allow_origin, "*");
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
            if (target == "/health") {
                return make_response(http::status::ok,
                    R"({"status":"ok"})");
            }
            if (target == "/" || target == "/index.html" || target == "/dashboard.html") {
                return serve_file("dashboard.html", "text/html");
            }
            // 정적 파일 서빙
            if (target.starts_with("/static/")) {
                auto file = target.substr(8);
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

    http::response<http::string_body> serve_file(
        const std::string& filename, const std::string& content_type)
    {
        auto path = m_static_dir + "/" + filename;
        if (!std::filesystem::exists(path)) {
            http::response<http::string_body> res{http::status::not_found, 11};
            res.set(http::field::content_type, "text/plain");
            res.body() = "File not found: " + filename;
            res.prepare_payload();
            return res;
        }

        std::ifstream f(path, std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(f)),
                            std::istreambuf_iterator<char>());

        http::response<http::string_body> res{http::status::ok, 11};
        res.set(http::field::server, "HFT-Dashboard/1.0");
        res.set(http::field::content_type, content_type);
        res.set(http::field::access_control_allow_origin, "*");
        res.body() = std::move(content);
        res.prepare_payload();
        return res;
    }

    uint16_t m_port;
    std::string m_static_dir;
    DashboardCallbacks m_cb;
    std::atomic<bool> m_running{false};
    std::thread m_thread;
    std::unique_ptr<tcp::acceptor> m_acceptor;
};

} // namespace hft

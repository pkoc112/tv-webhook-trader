// ============================================================================
// webhook/http_server.hpp -- TradingView Webhook HTTP(S) 수신 서버
// v2.0 | 2026-03-13 | 보안 강화, IPv6 대응
//
// 변경 이력:
//   v1.0  초기 생성
//   v2.0  - constant-time 토큰 비교 (타이밍 공격 방지)
//         - IPv6-mapped IPv4 주소 정규화
//         - shared_ptr config (dangling ref 방지)
//         - 요청 카운터 + 기본 로깅
// ============================================================================
#pragma once

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <spdlog/spdlog.h>
#include <nlohmann/json.hpp>
#include <openssl/crypto.h>
#include <functional>
#include <memory>
#include <string>
#include <set>
#include <atomic>
#include <mutex>
#include <map>
#include <deque>

#include "core/types.hpp"
#include "core/spsc_queue.hpp"
#include "webhook/signal_types.hpp"

namespace beast = boost::beast;
namespace http  = beast::http;
namespace net   = boost::asio;
namespace ssl   = net::ssl;
using tcp = net::ip::tcp;

namespace hft {

struct WebhookConfig {
    std::string bind_address{"0.0.0.0"};
    uint16_t    port{8443};
    std::string ssl_cert_path;
    std::string ssl_key_path;
    std::string auth_token;
    std::set<std::string> allowed_ips;
    int         max_body_size{4096};
};

using SignalCallback = std::function<void(WebhookSignal&&)>;

// -- Constant-time string comparison --
inline bool constant_time_eq(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

// -- IPv6-mapped IPv4 정규화: "::ffff:1.2.3.4" -> "1.2.3.4" --
inline std::string normalize_ip(const std::string& ip) {
    const std::string prefix = "::ffff:";
    if (ip.size() > prefix.size() &&
        ip.compare(0, prefix.size(), prefix) == 0) {
        return ip.substr(prefix.size());
    }
    return ip;
}

// -- Per-IP rate limiter for webhook endpoint --
class WebhookRateLimiter {
public:
    // Returns true if allowed, false if rate limited
    bool check(const std::string& ip) {
        std::lock_guard<std::mutex> lock(m_mtx);
        auto now = std::chrono::steady_clock::now();
        auto cutoff = now - std::chrono::seconds(60);

        auto& timestamps = m_requests[ip];

        // Remove entries older than 60 seconds
        while (!timestamps.empty() && timestamps.front() < cutoff) {
            timestamps.pop_front();
        }

        if (static_cast<int>(timestamps.size()) >= m_max_per_minute) {
            return false;
        }

        timestamps.push_back(now);

        // Periodic cleanup of stale IPs (every 100 checks)
        if (++m_check_count % 100 == 0) {
            for (auto it = m_requests.begin(); it != m_requests.end(); ) {
                if (it->second.empty()) {
                    it = m_requests.erase(it);
                } else {
                    ++it;
                }
            }
        }

        return true;
    }

private:
    std::mutex m_mtx;
    std::map<std::string, std::deque<std::chrono::steady_clock::time_point>> m_requests;
    int m_max_per_minute{30};
    uint64_t m_check_count{0};
};

// -- SSL Session --
class WebhookSession : public std::enable_shared_from_this<WebhookSession> {
public:
    WebhookSession(beast::ssl_stream<beast::tcp_stream> stream,
                   std::shared_ptr<const WebhookConfig> config,
                   SignalCallback cb,
                   std::atomic<uint64_t>& req_counter,
                   WebhookRateLimiter& rate_limiter,
                   std::string client_ip)
        : m_stream(std::move(stream))
        , m_config(std::move(config))
        , m_callback(std::move(cb))
        , m_req_counter(req_counter)
        , m_rate_limiter(rate_limiter)
        , m_client_ip(std::move(client_ip)) {}

    void run() {
        beast::get_lowest_layer(m_stream).expires_after(std::chrono::seconds(10));
        m_stream.async_handshake(ssl::stream_base::server,
            [self = shared_from_this()](beast::error_code ec) {
                if (ec) { spdlog::warn("[HTTP] SSL handshake failed: {}", ec.message()); return; }
                self->do_read();
            });
    }

private:
    void do_read() {
        m_parser.emplace();
        m_parser->body_limit(m_config->max_body_size);

        beast::get_lowest_layer(m_stream).expires_after(std::chrono::seconds(10));
        http::async_read(m_stream, m_buffer, *m_parser,
            [self = shared_from_this()](beast::error_code ec, size_t) {
                if (ec) {
                    if (ec != beast::errc::not_connected)
                        spdlog::warn("[HTTP] Read error: {}", ec.message());
                    return;
                }
                self->handle_request(self->m_parser->release());
            });
    }

    void handle_request(http::request<http::string_body>&& req) {
        m_req_counter.fetch_add(1, std::memory_order_relaxed);

        // Health check
        if (req.method() == http::verb::get && req.target() == "/health") {
            return send_response(http::status::ok, R"({"status":"ok"})");
        }

        // POST /webhook only
        if (req.method() != http::verb::post || req.target() != "/webhook") {
            return send_response(http::status::not_found, R"({"error":"not found"})");
        }

        // Per-IP rate limiting
        if (!m_rate_limiter.check(m_client_ip)) {
            spdlog::warn("[HTTP] Rate limited IP: {}", m_client_ip);
            return send_response(http::status::too_many_requests, R"({"error":"rate limited"})");
        }

        // 시그널 파싱
        auto sig_opt = WebhookSignal::from_json(req.body());
        if (!sig_opt) {
            spdlog::warn("[HTTP] Invalid JSON payload");
            return send_response(http::status::bad_request, R"({"error":"invalid json"})");
        }

        auto& sig = *sig_opt;

        // Constant-time 토큰 인증
        if (!m_config->auth_token.empty() && !constant_time_eq(sig.token, m_config->auth_token)) {
            spdlog::warn("[HTTP] Auth failed from signal");
            return send_response(http::status::unauthorized, R"({"error":"unauthorized"})");
        }

        // 시그널 유효성 검사
        if (!sig.is_valid()) {
            spdlog::warn("[HTTP] Invalid signal: type={} action={} symbol={}",
                signal_type_str(sig.sig_type), sig.action, sig.symbol);
            return send_response(http::status::bad_request, R"({"error":"invalid signal"})");
        }

        spdlog::info("[HTTP] Signal received: {} {} {} @ {:.2f}",
            signal_type_str(sig.sig_type), sig.action, sig.symbol, sig.price);

        m_callback(std::move(sig));
        send_response(http::status::ok, R"({"status":"accepted"})");
    }

    void send_response(http::status status, std::string body) {
        auto res = std::make_shared<http::response<http::string_body>>(status, 11);
        res->set(http::field::content_type, "application/json");
        res->set(http::field::server, "TV-Webhook-Trader/2.0");
        res->body() = std::move(body);
        res->prepare_payload();

        http::async_write(m_stream, *res,
            [self = shared_from_this(), res](beast::error_code ec, size_t) {
                self->m_stream.async_shutdown(
                    [self](beast::error_code) {});
            });
    }

    beast::ssl_stream<beast::tcp_stream> m_stream;
    beast::flat_buffer m_buffer;
    std::optional<http::request_parser<http::string_body>> m_parser;
    std::shared_ptr<const WebhookConfig> m_config;
    SignalCallback m_callback;
    std::atomic<uint64_t>& m_req_counter;
    WebhookRateLimiter& m_rate_limiter;
    std::string m_client_ip;
};

// -- Listener --
class WebhookListener : public std::enable_shared_from_this<WebhookListener> {
public:
    WebhookListener(net::io_context& ioc, ssl::context& ctx,
                    tcp::endpoint ep,
                    std::shared_ptr<const WebhookConfig> config,
                    SignalCallback cb,
                    std::atomic<uint64_t>& req_counter,
                    WebhookRateLimiter& rate_limiter)
        : m_ioc(ioc), m_ctx(ctx), m_acceptor(ioc)
        , m_config(std::move(config)), m_callback(std::move(cb))
        , m_req_counter(req_counter), m_rate_limiter(rate_limiter)
    {
        beast::error_code ec;
        m_acceptor.open(ep.protocol(), ec);
        m_acceptor.set_option(net::socket_base::reuse_address(true), ec);
        m_acceptor.bind(ep, ec);
        if (ec) { spdlog::error("[HTTP] Bind failed: {}", ec.message()); return; }
        m_acceptor.listen(net::socket_base::max_listen_connections, ec);
        if (ec) { spdlog::error("[HTTP] Listen failed: {}", ec.message()); return; }
        spdlog::info("[HTTP] Listening on {}:{}", m_config->bind_address, m_config->port);
    }

    void run() { do_accept(); }

private:
    void do_accept() {
        m_acceptor.async_accept(
            [self = shared_from_this()](beast::error_code ec, tcp::socket socket) {
                if (ec) {
                    if (ec != net::error::operation_aborted)
                        spdlog::error("[HTTP] Accept error: {}", ec.message());
                    return;
                }

                // IP 필터링 (IPv6-mapped IPv4 정규화)
                auto remote_raw = socket.remote_endpoint().address().to_string();
                auto remote = normalize_ip(remote_raw);

                if (!self->m_config->allowed_ips.empty() &&
                    self->m_config->allowed_ips.find(remote) == self->m_config->allowed_ips.end()) {
                    spdlog::warn("[HTTP] Blocked IP: {} (raw: {})", remote, remote_raw);
                    socket.close();
                } else {
                    auto session = std::make_shared<WebhookSession>(
                        beast::ssl_stream<beast::tcp_stream>(std::move(socket), self->m_ctx),
                        self->m_config, self->m_callback, self->m_req_counter,
                        self->m_rate_limiter, remote);
                    session->run();
                }

                self->do_accept();
            });
    }

    net::io_context& m_ioc;
    ssl::context& m_ctx;
    tcp::acceptor m_acceptor;
    std::shared_ptr<const WebhookConfig> m_config;
    SignalCallback m_callback;
    std::atomic<uint64_t>& m_req_counter;
    WebhookRateLimiter& m_rate_limiter;
};

// -- 최상위 서버 클래스 --
class WebhookServer {
public:
    WebhookServer(const WebhookConfig& config, SignalCallback cb)
        : m_config(std::make_shared<const WebhookConfig>(config))
        , m_callback(std::move(cb))
        , m_ssl_ctx(ssl::context::tlsv12_server)
    {
        m_ssl_ctx.set_options(
            ssl::context::default_workarounds |
            ssl::context::no_sslv2 |
            ssl::context::no_sslv3 |
            ssl::context::single_dh_use);
        m_ssl_ctx.use_certificate_chain_file(config.ssl_cert_path);
        m_ssl_ctx.use_private_key_file(config.ssl_key_path, ssl::context::pem);
    }

    void run(int threads = 8) {
        auto ep = tcp::endpoint(net::ip::make_address(m_config->bind_address), m_config->port);
        auto listener = std::make_shared<WebhookListener>(
            m_ioc, m_ssl_ctx, ep, m_config, m_callback, m_total_requests, m_rate_limiter);
        listener->run();

        std::vector<std::thread> workers;
        for (int i = 0; i < threads; ++i) {
            workers.emplace_back([this] { m_ioc.run(); });
        }
        spdlog::info("[HTTP] Server running with {} threads", threads);

        for (auto& t : workers) t.join();
    }

    void stop() {
        m_ioc.stop();
        spdlog::info("[HTTP] Server stopped. Total requests: {}", m_total_requests.load());
    }

private:
    std::shared_ptr<const WebhookConfig> m_config;
    SignalCallback m_callback;
    net::io_context m_ioc;
    ssl::context m_ssl_ctx;
    std::atomic<uint64_t> m_total_requests{0};
    WebhookRateLimiter m_rate_limiter;
};

} // namespace hft

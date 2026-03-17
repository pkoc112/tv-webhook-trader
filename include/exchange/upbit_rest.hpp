// ============================================================================
// exchange/upbit_rest.hpp -- Upbit REST API 클라이언트 (현물)
// v1.0 | 2026-03-17 | 잔고 조회, 시세 조회, 매수/매도 주문
//
// 업비트 Open API v1
//   - 인증: JWT (HS256)
//   - Base URL: https://api.upbit.com
//   - 주문: POST /v1/orders
//   - 잔고: GET /v1/accounts
//   - 시세: GET /v1/ticker
// ============================================================================
#pragma once

#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <string>
#include <mutex>
#include <cmath>

#include "exchange/upbit_auth.hpp"

namespace beast = boost::beast;
namespace http  = beast::http;
namespace net   = boost::asio;
namespace ssl   = net::ssl;
using tcp = net::ip::tcp;
using json = nlohmann::json;

namespace hft {

struct UpbitRestConfig {
    std::string base_host{"api.upbit.com"};
    std::string base_port{"443"};
    int timeout_sec{10};
};

class UpbitRestClient {
public:
    UpbitRestClient(net::io_context& ioc, UpbitAuth auth, UpbitRestConfig config = {})
        : m_ioc(ioc), m_auth(std::move(auth)), m_config(std::move(config))
        , m_ssl_ctx(ssl::context::tlsv12_client)
    {
        m_ssl_ctx.set_default_verify_paths();
        m_ssl_ctx.set_verify_mode(ssl::verify_peer);
        m_ssl_ctx.set_verify_callback(ssl::host_name_verification(m_config.base_host));
    }

    // -- 잔고 조회 --
    json get_accounts() {
        auto resp = do_request("GET", "/v1/accounts", "", "");
        try { return json::parse(resp); }
        catch (...) { return json::array(); }
    }

    // -- KRW 잔고 조회 --
    double get_krw_balance() {
        auto accounts = get_accounts();
        if (accounts.is_array()) {
            for (auto& acc : accounts) {
                if (acc.value("currency", "") == "KRW") {
                    return std::stod(acc.value("balance", "0"));
                }
            }
        }
        return 0.0;
    }

    // -- 특정 코인 잔고 조회 --
    double get_coin_balance(const std::string& currency) {
        auto accounts = get_accounts();
        if (accounts.is_array()) {
            for (auto& acc : accounts) {
                if (acc.value("currency", "") == currency) {
                    return std::stod(acc.value("balance", "0"));
                }
            }
        }
        return 0.0;
    }

    // -- 시세 조회 --
    json get_ticker(const std::string& market) {
        std::string query = "markets=" + market;
        auto resp = do_request("GET", "/v1/ticker?" + query, "", "");
        try { return json::parse(resp); }
        catch (...) { return json::array(); }
    }

    // -- 시장가 매수 (KRW 금액 기준) --
    json place_buy_market(const std::string& market, double krw_amount) {
        std::string query = "market=" + market +
                           "&side=bid" +
                           "&price=" + std::to_string(static_cast<int64_t>(krw_amount)) +
                           "&ord_type=price";  // 시장가 매수

        json body;
        body["market"]   = market;
        body["side"]     = "bid";
        body["price"]    = std::to_string(static_cast<int64_t>(krw_amount));
        body["ord_type"] = "price";

        spdlog::info("[UPBIT] Buy market={} amount={}KRW", market, krw_amount);

        auto resp = do_request("POST", "/v1/orders", body.dump(), query);
        try { return json::parse(resp); }
        catch (...) { return json{{"error", "parse_failed"}}; }
    }

    // -- 시장가 매도 (수량 기준) --
    json place_sell_market(const std::string& market, double volume) {
        // 수량 포맷 (소수점 8자리)
        char vol_buf[32];
        std::snprintf(vol_buf, sizeof(vol_buf), "%.8f", volume);

        std::string query = "market=" + market +
                           "&side=ask" +
                           "&volume=" + std::string(vol_buf) +
                           "&ord_type=market";  // 시장가 매도

        json body;
        body["market"]   = market;
        body["side"]     = "ask";
        body["volume"]   = std::string(vol_buf);
        body["ord_type"] = "market";

        spdlog::info("[UPBIT] Sell market={} volume={}", market, vol_buf);

        auto resp = do_request("POST", "/v1/orders", body.dump(), query);
        try { return json::parse(resp); }
        catch (...) { return json{{"error", "parse_failed"}}; }
    }

    // -- 마켓 정보 조회 (거래 가능 심볼 목록) --
    json get_markets() {
        auto resp = do_request("GET", "/v1/market/all?isDetails=true", "", "");
        try { return json::parse(resp); }
        catch (...) { return json::array(); }
    }

    void disconnect() {
        std::lock_guard lock(m_conn_mtx);
        close_connection();
    }

private:
    void ensure_connected() {
        if (m_stream && m_connected) return;
        close_connection();

        tcp::resolver resolver(m_ioc);
        auto results = resolver.resolve(m_config.base_host, m_config.base_port);

        m_stream = std::make_unique<beast::ssl_stream<beast::tcp_stream>>(m_ioc, m_ssl_ctx);
        SSL_set_tlsext_host_name(m_stream->native_handle(), m_config.base_host.c_str());

        beast::get_lowest_layer(*m_stream).expires_after(
            std::chrono::seconds(m_config.timeout_sec));

        beast::get_lowest_layer(*m_stream).connect(results);
        m_stream->handshake(ssl::stream_base::client);
        m_connected = true;

        spdlog::debug("[UPBIT] TLS connection established");
    }

    void close_connection() {
        if (m_stream) {
            beast::error_code ec;
            m_stream->shutdown(ec);
            m_stream.reset();
        }
        m_connected = false;
    }

    // query_for_auth: JWT 서명에 포함할 쿼리 문자열 (POST의 body params)
    std::string do_request(const std::string& method, const std::string& path,
                           const std::string& body, const std::string& query_for_auth = "") {
        std::lock_guard lock(m_conn_mtx);

        for (int attempt = 0; attempt < 2; ++attempt) {
            try {
                ensure_connected();

                // JWT 토큰 생성
                std::string auth_token;
                if (m_auth.is_configured()) {
                    if (!query_for_auth.empty()) {
                        auth_token = m_auth.auth_header(query_for_auth);
                    } else {
                        auth_token = m_auth.auth_header();
                    }
                }

                http::request<http::string_body> req(
                    method == "POST" ? http::verb::post : http::verb::get,
                    path, 11);
                req.set(http::field::host, m_config.base_host);
                req.set(http::field::connection, "keep-alive");
                req.set(http::field::content_type, "application/json");

                if (!auth_token.empty()) {
                    req.set(http::field::authorization, auth_token);
                }

                if (!body.empty()) req.body() = body;
                req.prepare_payload();

                beast::get_lowest_layer(*m_stream).expires_after(
                    std::chrono::seconds(m_config.timeout_sec));

                http::write(*m_stream, req);

                beast::flat_buffer buffer;
                http::response<http::string_body> res;
                http::read(*m_stream, buffer, res);

                spdlog::debug("[UPBIT] Response [{}]: {}",
                    static_cast<int>(res.result()),
                    res.body().substr(0, 200));

                auto conn = res[http::field::connection];
                if (conn == "close") {
                    close_connection();
                }

                return res.body();

            } catch (const std::exception& e) {
                spdlog::warn("[UPBIT] Request failed (attempt {}): {}", attempt + 1, e.what());
                close_connection();
                if (attempt == 0) continue;
                spdlog::error("[UPBIT] Request failed after retry: {}", e.what());
                return R"({"error":"connection_error"})";
            }
        }
        return R"({"error":"unreachable"})";
    }

    net::io_context& m_ioc;
    UpbitAuth m_auth;
    UpbitRestConfig m_config;
    ssl::context m_ssl_ctx;

    std::mutex m_conn_mtx;
    std::unique_ptr<beast::ssl_stream<beast::tcp_stream>> m_stream;
    bool m_connected{false};
};

} // namespace hft

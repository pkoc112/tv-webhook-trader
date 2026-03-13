// ============================================================================
// exchange/bitget_rest.hpp -- Bitget V2 REST API 클라이언트 (선물 주문)
// v2.0 | 2026-03-13 | 연결 재사용, TLS 검증, holdSide 수정
//
// 변경 이력:
//   v1.0  초기 생성
//   v2.0  - 영속 TLS 연결 + 자동 재연결
//         - TLS 인증서 검증 활성화
//         - holdSide 버그 수정 (short TP/SL 지원)
//         - 에러 메시지 안전한 포맷팅
//         - 요청 타임아웃 설정
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
#include <optional>
#include <mutex>

#include "core/types.hpp"
#include "exchange/bitget_auth.hpp"

namespace beast = boost::beast;
namespace http  = beast::http;
namespace net   = boost::asio;
namespace ssl   = net::ssl;
using tcp = net::ip::tcp;
using json = nlohmann::json;

namespace hft {

struct BitgetRestConfig {
    std::string base_host{"api.bitget.com"};
    std::string base_port{"443"};
    int timeout_sec{10};
};

class BitgetRestClient {
public:
    BitgetRestClient(net::io_context& ioc, BitgetAuth auth, BitgetRestConfig config = {})
        : m_ioc(ioc), m_auth(std::move(auth)), m_config(std::move(config))
        , m_ssl_ctx(ssl::context::tlsv12_client)
    {
        // TLS 인증서 검증 활성화
        m_ssl_ctx.set_default_verify_paths();
        m_ssl_ctx.set_verify_mode(ssl::verify_peer);
        m_ssl_ctx.set_verify_callback(ssl::host_name_verification(m_config.base_host));
    }

    // -- 선물 주문 실행 --
    OrderResponse place_futures_order(const OrderRequest& req) {
        json body;
        body["symbol"]      = req.symbol.str();
        body["productType"] = "USDT-FUTURES";
        body["marginMode"]  = "crossed";
        body["marginCoin"]  = "USDT";
        body["side"]        = (req.side == Side::Buy) ? "buy" : "sell";
        body["tradeSide"]   = (req.trade_side == TradeSide::Open) ? "open" : "close";
        body["orderType"]   = (req.order_type == OrderType::Market) ? "market" : "limit";
        body["size"]        = std::to_string(req.quantity);
        body["clientOid"]   = std::to_string(req.client_order_id);

        if (req.order_type == OrderType::Limit) {
            body["price"] = std::to_string(req.price);
            body["force"] = "gtc";
        } else {
            body["force"] = "ioc";
        }

        auto body_str = body.dump();
        const std::string path = "/api/v2/mix/order/place-order";

        spdlog::info("[REST] Placing order: {} {} {} size={} oid={}",
            body["side"].get<std::string>(), body["tradeSide"].get<std::string>(),
            req.symbol.str(), req.quantity, req.client_order_id);

        auto resp_str = do_request("POST", path, body_str);
        return parse_order_response(resp_str, req.client_order_id);
    }

    // -- TP/SL 설정 (holdSide 수정 완료) --
    void place_tpsl(const std::string& symbol, const std::string& plan_type,
                    double trigger_price, double size, const std::string& hold_side) {
        json body;
        body["symbol"]       = symbol;
        body["productType"]  = "USDT-FUTURES";
        body["marginMode"]   = "crossed";
        body["marginCoin"]   = "USDT";
        body["planType"]     = plan_type;
        body["triggerPrice"]  = std::to_string(trigger_price);
        body["triggerType"]   = "mark_price";
        body["size"]          = std::to_string(size);
        body["holdSide"]      = hold_side;  // "long" or "short"

        auto body_str = body.dump();
        const std::string path = "/api/v2/mix/order/place-tpsl-order";

        spdlog::info("[REST] Setting {} for {} @ {:.2f} hold={}",
            plan_type, symbol, trigger_price, hold_side);

        auto resp = do_request("POST", path, body_str);
        try {
            auto j = json::parse(resp);
            auto code = j.value("code", "99999");
            if (code != "00000") {
                spdlog::error("[REST] TPSL failed: code={} msg={}", code, j.value("msg", "unknown"));
            }
        } catch (...) {
            spdlog::error("[REST] TPSL response parse failed");
        }
    }

    // -- 레버리지 설정 (Bitget V2) --
    bool set_leverage(const std::string& symbol, int leverage,
                      const std::string& /*margin_mode*/ = "crossed",
                      const std::string& hold_side = "") {
        json body;
        body["symbol"]      = symbol;
        body["productType"] = "USDT-FUTURES";
        body["marginCoin"]  = "USDT";
        body["leverage"]    = std::to_string(leverage);

        if (!hold_side.empty()) {
            body["holdSide"] = hold_side;
        }

        auto body_str = body.dump();
        const std::string path = "/api/v2/mix/account/set-leverage";

        spdlog::info("[REST] Setting leverage: {} {}x hold={}", symbol, leverage, hold_side);

        auto resp = do_request("POST", path, body_str);
        try {
            auto j = json::parse(resp);
            auto code = j.value("code", "99999");
            if (code == "00000") {
                spdlog::info("[REST] Leverage set: {} {}x", symbol, leverage);
                return true;
            } else {
                spdlog::error("[REST] Leverage failed: {} code={} msg={}",
                    symbol, code, j.value("msg", "unknown"));
                return false;
            }
        } catch (...) {
            spdlog::error("[REST] Leverage response parse failed for {}", symbol);
            return false;
        }
    }

    // -- 포지션 조회 --
    json get_positions(const std::string& symbol = "") {
        std::string path = "/api/v2/mix/position/all-position?productType=USDT-FUTURES";
        if (!symbol.empty()) path += "&symbol=" + symbol;
        auto resp = do_request("GET", path, "");
        try { return json::parse(resp); }
        catch (...) { return json{}; }
    }

    // -- 계정 잔고 조회 --
    json get_account() {
        const std::string path = "/api/v2/mix/account/account?symbol=BTCUSDT&productType=USDT-FUTURES&marginCoin=USDT";
        auto resp = do_request("GET", path, "");
        try { return json::parse(resp); }
        catch (...) { return json{}; }
    }

    void disconnect() {
        std::lock_guard lock(m_conn_mtx);
        close_connection();
    }

private:
    // -- 영속 연결 관리 --
    void ensure_connected() {
        if (m_stream && m_connected) return;
        close_connection();

        tcp::resolver resolver(m_ioc);
        auto results = resolver.resolve(m_config.base_host, m_config.base_port);

        m_stream = std::make_unique<beast::ssl_stream<beast::tcp_stream>>(m_ioc, m_ssl_ctx);
        SSL_set_tlsext_host_name(m_stream->native_handle(), m_config.base_host.c_str());

        // 타임아웃 설정
        beast::get_lowest_layer(*m_stream).expires_after(
            std::chrono::seconds(m_config.timeout_sec));

        beast::get_lowest_layer(*m_stream).connect(results);
        m_stream->handshake(ssl::stream_base::client);
        m_connected = true;

        spdlog::debug("[REST] TLS connection established to {}", m_config.base_host);
    }

    void close_connection() {
        if (m_stream) {
            beast::error_code ec;
            m_stream->shutdown(ec);
            m_stream.reset();
        }
        m_connected = false;
    }

    std::string do_request(const std::string& method, const std::string& path,
                           const std::string& body) {
        std::lock_guard lock(m_conn_mtx);

        for (int attempt = 0; attempt < 2; ++attempt) {
            try {
                ensure_connected();

                // 헤더 생성
                auto auth_headers = m_auth.headers(method, path, body);

                http::request<http::string_body> req(
                    method == "POST" ? http::verb::post : http::verb::get,
                    path, 11);
                req.set(http::field::host, m_config.base_host);
                req.set(http::field::connection, "keep-alive");
                for (auto& [k, v] : auth_headers) req.set(k, v);
                if (!body.empty()) req.body() = body;
                req.prepare_payload();

                // 타임아웃 설정
                beast::get_lowest_layer(*m_stream).expires_after(
                    std::chrono::seconds(m_config.timeout_sec));

                http::write(*m_stream, req);

                beast::flat_buffer buffer;
                http::response<http::string_body> res;
                http::read(*m_stream, buffer, res);

                spdlog::debug("[REST] Response [{}]: {}",
                    static_cast<int>(res.result()),
                    res.body().substr(0, 200));

                // Connection: close 이면 연결 해제
                auto conn = res[http::field::connection];
                if (conn == "close") {
                    close_connection();
                }

                return res.body();

            } catch (const std::exception& e) {
                spdlog::warn("[REST] Request failed (attempt {}): {}", attempt + 1, e.what());
                close_connection();
                if (attempt == 0) continue;  // 한 번 재시도
                spdlog::error("[REST] Request failed after retry: {}", e.what());
                return R"({"code":"99999","msg":"connection_error"})";
            }
        }
        return R"({"code":"99999","msg":"unreachable"})";
    }

    OrderResponse parse_order_response(const std::string& body, OrderId client_oid) {
        OrderResponse resp{};
        resp.client_order_id = client_oid;
        resp.updated_at = now_ns();

        try {
            auto j = json::parse(body);
            auto code = j.value("code", "99999");
            if (code == "00000") {
                resp.status = OrderStatus::New;
                auto& data = j["data"];
                if (data.contains("orderId")) {
                    resp.exchange_order_id = std::stoull(data["orderId"].get<std::string>());
                }
                spdlog::info("[REST] Order placed: oid={} exchange_id={}",
                    client_oid, resp.exchange_order_id);
            } else {
                resp.status = OrderStatus::Rejected;
                resp.error_code = std::stoi(code);
                auto msg = j.value("msg", "unknown");
                auto len = std::min(msg.size(), size_t{127});
                std::copy_n(msg.data(), len, resp.error_msg.data());
                spdlog::error("[REST] Order rejected: code={} msg={}", code, msg);
            }
        } catch (const std::exception& e) {
            resp.status = OrderStatus::Rejected;
            resp.error_code = 99999;
            spdlog::error("[REST] Parse response failed: {}", e.what());
        }
        return resp;
    }

    net::io_context& m_ioc;
    BitgetAuth m_auth;
    BitgetRestConfig m_config;
    ssl::context m_ssl_ctx;

    // 영속 연결
    std::mutex m_conn_mtx;
    std::unique_ptr<beast::ssl_stream<beast::tcp_stream>> m_stream;
    bool m_connected{false};
};

} // namespace hft

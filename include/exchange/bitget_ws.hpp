// ============================================================================
// exchange/bitget_ws.hpp -- Bitget V2 WebSocket Private 클라이언트
// v1.1 | 2026-03-14 | 실시간 잔고/포지션/체결 수신
//
// 기능:
//   - TLS WebSocket 연결 (wss://ws.bitget.com/v2/ws/private)
//   - HMAC-SHA256 로그인 인증
//   - account, positions, orders 채널 구독
//   - Ping/Pong 자동 유지 + 헬스체크 (10초 pong 타임아웃)
//   - 무제한 자동 재연결 (지수 백오프, 최대 60초)
//   - 재연결 시 자동 재인증 + 채널 재구독
//   - 콜백을 통한 실시간 업데이트
// ============================================================================
#pragma once

#include <boost/beast/core.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <string>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>

#include "core/types.hpp"
#include "exchange/bitget_auth.hpp"

namespace beast = boost::beast;
namespace websocket = beast::websocket;
namespace net = boost::asio;
namespace ssl = net::ssl;
using tcp = net::ip::tcp;
using json = nlohmann::json;

namespace hft {

// -- 콜백 타입 --
struct WsAccountUpdate {
    double available{0.0};
    double equity{0.0};
    double unrealized_pnl{0.0};
    std::string margin_coin;
};

struct WsPositionUpdate {
    std::string symbol;
    std::string hold_side;   // "long" or "short"
    double size{0.0};
    double avg_price{0.0};
    double unrealized_pnl{0.0};
    double realized_pnl{0.0};
    double leverage{0.0};
    double margin{0.0};
};

struct WsOrderUpdate {
    std::string symbol;
    std::string order_id;
    std::string client_oid;
    std::string side;           // "buy" or "sell"
    std::string trade_side;     // "open" or "close"
    std::string status;         // "new", "partial-fill", "full-fill", "cancelled"
    double price{0.0};
    double size{0.0};
    double filled_size{0.0};
    double filled_amount{0.0};  // USDT
    double fee{0.0};
};

using OnAccountUpdate  = std::function<void(const WsAccountUpdate&)>;
using OnPositionUpdate = std::function<void(const WsPositionUpdate&)>;
using OnOrderUpdate    = std::function<void(const WsOrderUpdate&)>;

class BitgetWSClient {
public:
    BitgetWSClient(BitgetAuth auth,
                   OnAccountUpdate  on_account  = nullptr,
                   OnPositionUpdate on_position = nullptr,
                   OnOrderUpdate    on_order    = nullptr)
        : m_auth(std::move(auth))
        , m_on_account(std::move(on_account))
        , m_on_position(std::move(on_position))
        , m_on_order(std::move(on_order))
    {}

    ~BitgetWSClient() { stop(); }

    void start() {
        if (m_running.load()) return;
        m_running.store(true);
        m_thread = std::thread([this] { run_loop(); });
        spdlog::info("[WS] Client started");
    }

    void stop() {
        m_running.store(false);
        if (m_thread.joinable()) {
            m_thread.join();
        }
        spdlog::info("[WS] Client stopped");
    }

    [[nodiscard]] bool is_connected() const { return m_connected.load(); }

private:
    static constexpr const char* WS_HOST = "ws.bitget.com";
    static constexpr const char* WS_PORT = "443";
    static constexpr const char* WS_PATH = "/v2/ws/private";
    static constexpr int PING_INTERVAL_SEC = 25;
    static constexpr int PONG_TIMEOUT_SEC = 10;
    static constexpr int BACKOFF_INITIAL_SEC = 1;
    static constexpr int BACKOFF_MAX_SEC = 60;

    void run_loop() {
        int reconnect_count = 0;
        int backoff_sec = BACKOFF_INITIAL_SEC;

        while (m_running.load()) {
            try {
                connect_and_run();
                // 정상 종료 (m_running = false) -- 백오프 리셋
                reconnect_count = 0;
                backoff_sec = BACKOFF_INITIAL_SEC;
            } catch (const std::exception& e) {
                bool was_connected = m_connected.exchange(false);
                if (!m_running.load()) break;

                // 연결 성공 후 끊긴 경우 백오프 리셋 (24시간 자동 끊김 등)
                if (was_connected) {
                    backoff_sec = BACKOFF_INITIAL_SEC;
                    reconnect_count = 0;
                    spdlog::info("[WS] Was connected -- backoff reset to {}s", backoff_sec);
                }

                ++reconnect_count;
                spdlog::warn("[WS] Disconnected: {} | Reconnect attempt #{} in {}s",
                    e.what(), reconnect_count, backoff_sec);

                // 재연결 대기 (중간에 stop 가능하도록 1초씩)
                for (int i = 0; i < backoff_sec && m_running.load(); ++i) {
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }

                // 지수 백오프: 1 -> 2 -> 4 -> 8 -> 16 -> 32 -> 60 -> 60 ...
                backoff_sec = std::min(backoff_sec * 2, BACKOFF_MAX_SEC);
            }
        }
        m_connected.store(false);
    }

    void connect_and_run() {
        net::io_context ioc;
        ssl::context ssl_ctx(ssl::context::tlsv12_client);
        ssl_ctx.set_default_verify_paths();

        // DNS 확인
        tcp::resolver resolver(ioc);
        auto results = resolver.resolve(WS_HOST, WS_PORT);

        // TLS + WebSocket 스트림 생성
        websocket::stream<beast::ssl_stream<beast::tcp_stream>> ws(ioc, ssl_ctx);
        SSL_set_tlsext_host_name(ws.next_layer().native_handle(), WS_HOST);

        // TCP 연결
        beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(10));
        beast::get_lowest_layer(ws).connect(results);

        // TLS 핸드셰이크
        ws.next_layer().handshake(ssl::stream_base::client);

        // WebSocket 핸드셰이크
        beast::get_lowest_layer(ws).expires_never();
        ws.set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));
        ws.set_option(websocket::stream_base::decorator([](websocket::request_type& req) {
            req.set(beast::http::field::user_agent, "hft-cpp-trader/3.0");
        }));
        ws.handshake(WS_HOST, WS_PATH);

        spdlog::info("[WS] Connected to {}{}", WS_HOST, WS_PATH);

        // 로그인
        send_login(ws);

        // 로그인 응답 대기
        beast::flat_buffer buf;
        ws.read(buf);
        auto login_resp = json::parse(beast::buffers_to_string(buf.data()));
        buf.clear();

        if (login_resp.value("event", "") != "login" ||
            login_resp.value("code", 1) != 0) {
            throw std::runtime_error("Login failed: " + login_resp.dump());
        }
        spdlog::info("[WS] Login successful");

        // 채널 구독
        send_subscribe(ws);

        // 구독 응답 대기
        ws.read(buf);
        auto sub_resp = json::parse(beast::buffers_to_string(buf.data()));
        buf.clear();
        spdlog::info("[WS] Subscribed: {}", sub_resp.value("event", "unknown"));

        m_connected.store(true);
        spdlog::info("[WS] Fully authenticated and subscribed -- resetting backoff");

        // 메시지 수신 루프
        auto last_ping = std::chrono::steady_clock::now();
        auto last_pong = std::chrono::steady_clock::now();
        bool pong_pending = false;

        while (m_running.load()) {
            auto now = std::chrono::steady_clock::now();

            // Ping 주기 체크
            auto ping_elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_ping).count();
            if (ping_elapsed >= PING_INTERVAL_SEC) {
                ws.write(net::buffer(std::string("ping")));
                last_ping = now;
                if (!pong_pending) {
                    pong_pending = true;
                }
            }

            // Pong 헬스체크: pong을 기다리는 중인데 PONG_TIMEOUT_SEC 초과 시 강제 재연결
            if (pong_pending) {
                auto pong_wait = std::chrono::duration_cast<std::chrono::seconds>(now - last_ping).count();
                if (pong_wait >= PONG_TIMEOUT_SEC) {
                    spdlog::error("[WS] No pong received within {}s -- forcing reconnect", PONG_TIMEOUT_SEC);
                    throw std::runtime_error("Pong timeout: no response within " +
                        std::to_string(PONG_TIMEOUT_SEC) + "s");
                }
            }

            // 논블로킹 읽기 (타임아웃 설정)
            beast::get_lowest_layer(ws).expires_after(std::chrono::seconds(5));
            beast::error_code ec;
            ws.read(buf, ec);

            if (ec == beast::error::timeout) {
                buf.clear();
                continue;  // 타임아웃은 정상
            }
            if (ec) {
                throw beast::system_error(ec);
            }

            auto msg = beast::buffers_to_string(buf.data());
            buf.clear();

            // pong 응답 처리 -- 헬스체크 리셋
            if (msg == "pong") {
                pong_pending = false;
                last_pong = now;
                continue;
            }

            // JSON 파싱 및 처리
            try {
                auto j = json::parse(msg);
                handle_message(j);
            } catch (const json::parse_error&) {
                spdlog::debug("[WS] Non-JSON message: {}", msg.substr(0, 100));
            }
        }

        // 정상 종료
        beast::error_code ec;
        ws.close(websocket::close_code::normal, ec);
    }

    void send_login(auto& ws) {
        auto ts = std::to_string(now_ms() / 1000);  // 초 단위
        auto sig = m_auth.sign(ts, "GET", "/user/verify");

        json login = {
            {"op", "login"},
            {"args", json::array({
                {
                    {"apiKey", get_api_key()},
                    {"passphrase", get_passphrase()},
                    {"timestamp", ts},
                    {"sign", sig}
                }
            })}
        };
        ws.write(net::buffer(login.dump()));
    }

    void send_subscribe(auto& ws) {
        json sub = {
            {"op", "subscribe"},
            {"args", json::array({
                {{"instType", "USDT-FUTURES"}, {"channel", "account"}, {"coin", "default"}},
                {{"instType", "USDT-FUTURES"}, {"channel", "positions"}, {"instId", "default"}},
                {{"instType", "USDT-FUTURES"}, {"channel", "orders"}, {"instId", "default"}}
            })}
        };
        ws.write(net::buffer(sub.dump()));
    }

    void handle_message(const json& j) {
        if (!j.contains("arg") || !j.contains("data")) return;

        auto channel = j["arg"].value("channel", "");
        auto& data = j["data"];

        if (channel == "account") {
            for (auto& item : data) {
                WsAccountUpdate upd;
                upd.margin_coin = item.value("marginCoin", "USDT");
                if (item.contains("available"))
                    upd.available = safe_stod(item["available"]);
                if (item.contains("accountEquity"))
                    upd.equity = safe_stod(item["accountEquity"]);
                if (item.contains("unrealizedPL"))
                    upd.unrealized_pnl = safe_stod(item["unrealizedPL"]);

                spdlog::info("[WS] Account: available={:.2f} equity={:.2f} uPnL={:.2f}",
                    upd.available, upd.equity, upd.unrealized_pnl);

                if (m_on_account) m_on_account(upd);
            }
        }
        else if (channel == "positions") {
            for (auto& item : data) {
                WsPositionUpdate upd;
                upd.symbol = item.value("instId", "");
                upd.hold_side = item.value("holdSide", "");
                if (item.contains("total"))
                    upd.size = safe_stod(item["total"]);
                if (item.contains("averageOpenPrice"))
                    upd.avg_price = safe_stod(item["averageOpenPrice"]);
                if (item.contains("unrealizedPL"))
                    upd.unrealized_pnl = safe_stod(item["unrealizedPL"]);
                if (item.contains("leverage"))
                    upd.leverage = safe_stod(item["leverage"]);
                if (item.contains("margin"))
                    upd.margin = safe_stod(item["margin"]);
                if (item.contains("achievedProfits"))
                    upd.realized_pnl = safe_stod(item["achievedProfits"]);

                spdlog::debug("[WS] Position: {} {} sz={:.4f} uPnL={:.4f} rPnL={:.4f}",
                    upd.symbol, upd.hold_side, upd.size, upd.unrealized_pnl, upd.realized_pnl);

                if (m_on_position) m_on_position(upd);
            }
        }
        else if (channel == "orders") {
            for (auto& item : data) {
                WsOrderUpdate upd;
                upd.symbol = item.value("instId", "");
                upd.order_id = item.value("orderId", "");
                upd.client_oid = item.value("clientOid", "");
                upd.side = item.value("side", "");
                upd.trade_side = item.value("tradeSide", "");
                upd.status = item.value("status", "");
                if (item.contains("price"))
                    upd.price = safe_stod(item["price"]);
                if (item.contains("size"))
                    upd.size = safe_stod(item["size"]);
                if (item.contains("baseVolume"))
                    upd.filled_size = safe_stod(item["baseVolume"]);
                if (item.contains("quoteVolume"))
                    upd.filled_amount = safe_stod(item["quoteVolume"]);
                if (item.contains("fee"))
                    upd.fee = safe_stod(item["fee"]);

                spdlog::info("[WS] Order: {} {} {} {} sz={:.4f} filled={:.4f} ${:.2f}",
                    upd.symbol, upd.side, upd.trade_side, upd.status,
                    upd.size, upd.filled_size, upd.filled_amount);

                if (m_on_order) m_on_order(upd);
            }
        }
    }

    static double safe_stod(const json& val) {
        if (val.is_number()) return val.get<double>();
        if (val.is_string()) {
            auto s = val.get<std::string>();
            if (s.empty()) return 0.0;
            try { return std::stod(s); } catch (...) { return 0.0; }
        }
        return 0.0;
    }

    // BitgetAuth에서 키/패스프레이즈 가져오기
    // (BitgetAuth에 getter가 없으므로 headers()에서 추출)
    std::string get_api_key() const {
        auto h = m_auth.headers("GET", "/");
        return h["ACCESS-KEY"];
    }
    std::string get_passphrase() const {
        auto h = m_auth.headers("GET", "/");
        return h["ACCESS-PASSPHRASE"];
    }

    BitgetAuth m_auth;
    OnAccountUpdate  m_on_account;
    OnPositionUpdate m_on_position;
    OnOrderUpdate    m_on_order;

    std::thread m_thread;
    std::atomic<bool> m_running{false};
    std::atomic<bool> m_connected{false};
};

} // namespace hft

// ============================================================================
// webhook/signal_types.hpp -- SFX TradingView 웹훅 시그널 파서
// v3.0 | 2026-03-13 | SFX 시그널 타입 분류, 주문 수량 설정
//
// 변경 이력:
//   v1.0  초기 생성 (범용 포맷)
//   v2.0  SFX 인디케이터 포맷 전용 파서로 재작성
//   v3.0  - SignalType enum 추가 (entry/tp/sl/re_entry)
//         - TP/SL/RE 시그널 정상 처리
//         - default_size 지원 (config에서 주입)
//         - entry_price 필드 추가
//         - 주문 중복 방지용 fingerprint
// ============================================================================
#pragma once

#include <string>
#include <optional>
#include <cmath>
#include <functional>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <fmt/format.h>
#include "core/types.hpp"

namespace hft {

using json = nlohmann::json;

enum class SignalType : uint8_t {
    Entry = 0,
    TP,
    SL,
    ReEntry,
    Unknown
};

inline const char* signal_type_str(SignalType t) {
    switch (t) {
        case SignalType::Entry:   return "entry";
        case SignalType::TP:      return "tp";
        case SignalType::SL:      return "sl";
        case SignalType::ReEntry: return "re_entry";
        default:                  return "unknown";
    }
}

struct WebhookSignal {
    // -- SFX 원본 필드 --
    std::string algorithm;
    std::string ticker;
    std::string ticker_full;
    int64_t     unix_time{0};
    std::string timestamp;
    std::string currency;
    std::string timeframe;
    std::string type;

    // -- 매매 시그널 --
    std::string alert;
    std::string signal_direction;
    int         current_rating{0};

    // -- 정규화된 필드 --
    std::string symbol;
    std::string action;         // "buy" / "sell"
    std::string trade_side;     // "open" / "close"
    SignalType  sig_type{SignalType::Unknown};
    std::string tp_level;       // "TP1" / "TP2" / "TP3" (TP 시그널일 때)
    double      price{0.0};
    double      entry_price{0.0};
    double      size{0.0};
    double      tp1{0.0};
    double      tp2{0.0};
    double      tp3{0.0};
    double      sl{0.0};
    std::string token;

    // -- 전략/시그널 소스 추적 --
    std::string strategy_name{"unknown"};

    // -- 수신 시각 --
    Timestamp   received_at{0};

    // -- 우선순위 (높을수록 먼저 처리) --
    // TF >= 60m: priority 3, TF >= 15m: 2, TF >= 5m: 1, TF 1m: 0
    int         priority{0};

    // -- JSON 파싱 --
    static std::optional<WebhookSignal> from_json(const std::string& body) {
        // 1차 시도: 원본 JSON 그대로 파싱
        auto j = json::parse(body, nullptr, false);
        if (!j.is_discarded()) {
            WebhookSignal sig;
            sig.received_at = now_ns();
            if (j.contains("algorithm") || j.contains("alert")) return parse_sfx(j, sig);
            return parse_generic(j, sig);
        }

        // 2차 시도: TradingView Custom JSON 이중 따옴표 수정
        // 전략: 모든 연속 따옴표("")를 단일 따옴표(")로 축소
        std::string sanitized;
        sanitized.reserve(body.size());
        for (size_t i = 0; i < body.size(); ++i) {
            sanitized += body[i];
            if (body[i] == '"') {
                while (i + 1 < body.size() && body[i + 1] == '"') {
                    ++i;
                }
            }
        }

        j = json::parse(sanitized, nullptr, false);
        if (!j.is_discarded()) {
            WebhookSignal sig;
            sig.received_at = now_ns();
            if (j.contains("algorithm") || j.contains("alert")) return parse_sfx(j, sig);
            return parse_generic(j, sig);
        }

        // 3차 시도: TradingView Custom JSON 병합 시 닫는 따옴표 누락 수정
        // TradingView가 Custom JSON {"token":"val"} 을 SFX JSON에 삽입할 때
        // ""token"": ""val 형태가 되어 축소 후 "token": "val, "next_key" 가 됨
        // → 토큰 값의 닫는 " 가 누락되어 JSON 파싱 실패
        // 수정: 문자열 값이 닫히지 않은 채 , " 패턴이 나오면 닫는 " 삽입
        std::string fixed = sanitized;
        size_t search_from = 0;
        bool did_fix = false;
        while (search_from < fixed.size()) {
            // JSON 키: "..." : "값시작 패턴 찾기
            auto colon = fixed.find(':', search_from);
            if (colon == std::string::npos) break;

            // : 뒤에 공백 건너뛰고 " 찾기
            size_t vs = colon + 1;
            while (vs < fixed.size() && fixed[vs] == ' ') ++vs;
            if (vs >= fixed.size() || fixed[vs] != '"') {
                search_from = colon + 1;
                continue;  // 값이 문자열이 아님 (숫자 등)
            }

            // 문자열 값 내부 스캔: 닫는 " 가 정상인지 확인
            size_t s = vs + 1;
            bool normal_close = false;
            while (s < fixed.size()) {
                if (fixed[s] == '\\' && s + 1 < fixed.size()) {
                    s += 2;  // 이스케이프 문자 건너뜀
                    continue;
                }
                if (fixed[s] == '"') {
                    normal_close = true;  // 정상 닫힘
                    break;
                }
                // , " 패턴: 닫는 따옴표 없이 다음 키가 시작됨
                if (fixed[s] == ',' && s + 1 < fixed.size()) {
                    size_t next = s + 1;
                    while (next < fixed.size() && fixed[next] == ' ') ++next;
                    if (next < fixed.size() && fixed[next] == '"') {
                        // 다음 키 시작 확인: "key": 패턴인지
                        auto closing_q = fixed.find('"', next + 1);
                        if (closing_q != std::string::npos) {
                            auto after_q = closing_q + 1;
                            while (after_q < fixed.size() && fixed[after_q] == ' ') ++after_q;
                            if (after_q < fixed.size() && fixed[after_q] == ':') {
                                // 확실히 다음 키-값 쌍 → 닫는 따옴표 삽입
                                fixed.insert(s, 1, '"');
                                did_fix = true;
                                spdlog::info("[Signal] Fixed missing closing quote at pos {}", s);
                                normal_close = true;
                                break;
                            }
                        }
                    }
                }
                ++s;
            }
            search_from = s + 1;
        }

        if (did_fix) {
            spdlog::info("[Signal] Fixed JSON: {}", fixed.substr(0, 500));
        }

        j = json::parse(fixed, nullptr, false);
        if (!j.is_discarded()) {
            WebhookSignal sig;
            sig.received_at = now_ns();
            if (j.contains("algorithm") || j.contains("alert")) return parse_sfx(j, sig);
            return parse_generic(j, sig);
        }

        spdlog::warn("[Signal] JSON parse failed after all attempts: {}", body.substr(0, 500));
        return std::nullopt;
    }

    // -- 유효성 검사 --
    [[nodiscard]] bool is_valid() const {
        if (symbol.empty()) return false;
        if (price <= 0.0) return false;

        switch (sig_type) {
            case SignalType::Entry:
            case SignalType::ReEntry:
                return (action == "buy" || action == "sell");
            case SignalType::TP:
            case SignalType::SL:
                return true;  // TP/SL은 action 없어도 유효
            default:
                return false;
        }
    }

    // -- Side 변환 --
    [[nodiscard]] Side get_side() const {
        return (action == "buy") ? Side::Buy : Side::Sell;
    }

    [[nodiscard]] TradeSide get_trade_side() const {
        return (trade_side == "close") ? TradeSide::Close : TradeSide::Open;
    }

    [[nodiscard]] std::string get_hold_side() const {
        return (action == "buy") ? "long" : "short";
    }

    [[nodiscard]] bool has_tp1() const { return tp1 > 0.0 && std::isfinite(tp1); }
    [[nodiscard]] bool has_tp2() const { return tp2 > 0.0 && std::isfinite(tp2); }
    [[nodiscard]] bool has_tp3() const { return tp3 > 0.0 && std::isfinite(tp3); }
    [[nodiscard]] bool has_sl()  const { return sl  > 0.0 && std::isfinite(sl); }

    // -- 중복 방지용 fingerprint --
    [[nodiscard]] size_t fingerprint() const {
        size_t h = 0;
        auto hash_combine = [&h](size_t v) {
            h ^= v + 0x9e3779b9 + (h << 6) + (h >> 2);
        };
        hash_combine(std::hash<std::string>{}(symbol));
        hash_combine(std::hash<std::string>{}(alert));
        hash_combine(std::hash<int64_t>{}(unix_time));
        hash_combine(std::hash<std::string>{}(timeframe));
        return h;
    }

    // -- 로그 출력 --
    [[nodiscard]] std::string to_log_string() const {
        auto s = fmt::format("[SFX] {} {} {} @ {:.2f} size={:.6f}",
            signal_type_str(sig_type), action, symbol, price, size);
        if (has_tp1()) s += fmt::format(" TP1={:.2f}", tp1);
        if (has_tp2()) s += fmt::format(" TP2={:.2f}", tp2);
        if (has_tp3()) s += fmt::format(" TP3={:.2f}", tp3);
        if (has_sl())  s += fmt::format(" SL={:.2f}", sl);
        s += fmt::format(" dir={} rating={} tf={} strat={}", signal_direction, current_rating, timeframe, strategy_name);
        return s;
    }

    // -- OrderRequest 변환 --
    [[nodiscard]] OrderRequest to_order_request(OrderId oid) const {
        OrderRequest req{};
        req.symbol = Symbol(symbol);
        req.side = get_side();
        req.trade_side = get_trade_side();
        req.order_type = OrderType::Market;
        req.tif = TimeInForce::IOC;
        req.price = price;
        req.quantity = size;
        req.client_order_id = oid;
        req.created_at = received_at;
        req.take_profit = tp1;
        req.stop_loss = sl;

        const char* pt = "USDT-FUTURES";
        std::copy_n(pt, 12, req.product_type.data());
        const char* mc = "USDT";
        std::copy_n(mc, 4, req.margin_coin.data());

        return req;
    }

private:
    // -- SFX 인디케이터 JSON 파싱 --
    static std::optional<WebhookSignal> parse_sfx(const json& j, WebhookSignal& sig) {
        sig.algorithm        = j.value("algorithm", "SFX");
        sig.ticker           = j.value("ticker", "");
        sig.ticker_full      = j.value("ticker_full", "");
        sig.unix_time        = j.value("unix_time", int64_t{0});
        sig.timestamp        = j.value("timestamp", "");
        sig.currency         = j.value("currency", "USDT");
        sig.timeframe        = j.value("timeframe", "");
        sig.type             = j.value("type", "crypto");

        // Priority based on timeframe (higher TF = higher priority)
        sig.priority = calc_priority(sig.timeframe);

        sig.alert            = j.value("alert", "");
        sig.signal_direction = j.value("signal_direction", "");

        // current_rating: 문자열 또는 숫자
        if (j.contains("current_rating")) {
            if (j["current_rating"].is_string()) {
                try { sig.current_rating = std::stoi(j["current_rating"].get<std::string>()); }
                catch (...) { sig.current_rating = 0; }
            } else {
                sig.current_rating = j["current_rating"].get<int>();
            }
        }

        // 심볼 정규화
        sig.symbol = normalize_symbol(sig.ticker);

        // 가격
        sig.price = j.value("close", 0.0);

        // entry_price (TP/SL 시그널에서 제공)
        sig.entry_price = parse_price_field(j, "entry_price");

        // TP / SL
        sig.tp1 = parse_price_field(j, "tp1");
        sig.tp2 = parse_price_field(j, "tp2");
        sig.tp3 = parse_price_field(j, "tp3");
        sig.sl  = parse_price_field(j, "sl");

        // 인증 토큰
        sig.token = j.value("token", j.value("secret", ""));

        // 전략/시그널 소스 추적
        sig.strategy_name = j.value("strategy", j.value("alert_name", "unknown"));

        // -- 시그널 종류 판별 --
        auto alert_upper = to_upper(sig.alert);

        if (alert_upper == "BUY" || alert_upper == "SELL") {
            sig.sig_type = SignalType::Entry;
            sig.action = to_lower(sig.alert);
            sig.trade_side = "open";
        } else if (alert_upper == "TP1" || alert_upper == "TP2" || alert_upper == "TP3") {
            sig.sig_type = SignalType::TP;
            sig.tp_level = alert_upper;
            sig.trade_side = "close";
            // TP 시그널의 action은 반대 방향
            if (sig.signal_direction == "bull") sig.action = "sell";       // 롱 청산
            else if (sig.signal_direction == "bear") sig.action = "buy";   // 숏 청산
        } else if (alert_upper == "SL" || alert_upper == "STOP_LOSS" ||
                   alert_upper == "STOPLOSS" || alert_upper == "AUTO_SL") {
            sig.sig_type = SignalType::SL;
            sig.trade_side = "close";
            if (sig.signal_direction == "bull") sig.action = "sell";
            else if (sig.signal_direction == "bear") sig.action = "buy";
        } else if (alert_upper == "RE" || alert_upper == "RE_ENTRY" ||
                   alert_upper == "REENTRY") {
            sig.sig_type = SignalType::ReEntry;
            sig.action = (sig.signal_direction == "bull") ? "buy" : "sell";
            sig.trade_side = "open";
        } else {
            sig.sig_type = SignalType::Unknown;
            spdlog::warn("[SFX] Unknown alert type: {}", sig.alert);
        }

        // Cross-validation
        if (sig.sig_type == SignalType::Entry) {
            if (sig.action == "buy" && sig.signal_direction == "bear") {
                spdlog::warn("[SFX] Mismatch: alert=buy but direction=bear");
            }
            if (sig.action == "sell" && sig.signal_direction == "bull") {
                spdlog::warn("[SFX] Mismatch: alert=sell but direction=bull");
            }
        }

        spdlog::info("{}", sig.to_log_string());
        return sig;
    }

    // -- 범용 JSON 파싱 --
    static std::optional<WebhookSignal> parse_generic(const json& j, WebhookSignal& sig) {
        sig.action     = j.value("action", "");
        sig.symbol     = j.value("symbol", "");
        sig.trade_side = j.value("trade_side", "open");
        sig.price      = j.value("price", 0.0);
        sig.size       = j.value("qty", 0.0);
        sig.tp1        = j.value("tp", j.value("tp1", 0.0));
        sig.sl         = j.value("sl", 0.0);
        sig.token      = j.value("token", j.value("secret", ""));
        sig.strategy_name = j.value("strategy", j.value("alert_name", "unknown"));
        sig.sig_type   = SignalType::Entry;

        if (sig.price == 0.0 && j.contains("price") && j["price"].is_string()) {
            try { sig.price = std::stod(j["price"].get<std::string>()); }
            catch (...) {}
        }

        return sig;
    }

    // Calculate signal priority from timeframe string
    // TF >= 60m: 3, >= 15m: 2, >= 5m: 1, else: 0
    static int calc_priority(const std::string& tf) {
        if (tf.empty()) return 0;
        try {
            int minutes = 0;
            if (tf.back() == 'h' || tf.back() == 'H') {
                minutes = std::stoi(tf.substr(0, tf.size() - 1)) * 60;
            } else if (tf.back() == 'd' || tf.back() == 'D') {
                minutes = std::stoi(tf.substr(0, tf.size() - 1)) * 1440;
            } else {
                minutes = std::stoi(tf);
            }
            if (minutes >= 60) return 3;
            if (minutes >= 15) return 2;
            if (minutes >= 5)  return 1;
            return 0;
        } catch (...) { return 0; }
    }

    static std::string normalize_symbol(const std::string& ticker) {
        std::string sym = ticker;
        if (sym.size() > 2 && sym.substr(sym.size() - 2) == ".P") {
            sym = sym.substr(0, sym.size() - 2);
        }
        auto colon = sym.find(':');
        if (colon != std::string::npos) {
            sym = sym.substr(colon + 1);
        }
        return sym;
    }

    static double parse_price_field(const json& j, const std::string& key) {
        if (!j.contains(key)) return 0.0;
        const auto& val = j[key];
        if (val.is_number()) {
            double v = val.get<double>();
            return std::isfinite(v) ? v : 0.0;
        }
        if (val.is_string()) {
            const auto& s = val.get<std::string>();
            if (s.empty() || s == "NaN" || s == "nan" || s == "null" || s == "0")
                return 0.0;
            try {
                double v = std::stod(s);
                return std::isfinite(v) ? v : 0.0;
            } catch (...) { return 0.0; }
        }
        return 0.0;
    }

    static std::string to_upper(const std::string& s) {
        std::string r = s;
        for (auto& c : r) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        return r;
    }

    static std::string to_lower(const std::string& s) {
        std::string r = s;
        for (auto& c : r) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        return r;
    }
};

} // namespace hft

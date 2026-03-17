// ============================================================================
// exchange/upbit_auth.hpp -- Upbit JWT 인증
// v1.0 | 2026-03-17 | HS256 JWT 토큰 생성
//
// 업비트 Open API는 JWT(JSON Web Token)를 사용.
// Header: {"alg":"HS256","typ":"JWT"}
// Payload: {"access_key":"...","nonce":"uuid","query_hash":"sha512(query)","query_hash_alg":"SHA512"}
// ============================================================================
#pragma once

#include <string>
#include <map>
#include <random>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/sha.h>

namespace hft {

class UpbitAuth {
public:
    UpbitAuth() = default;
    UpbitAuth(std::string access_key, std::string secret_key)
        : m_access_key(std::move(access_key)), m_secret_key(std::move(secret_key)) {}

    bool is_configured() const { return !m_access_key.empty() && !m_secret_key.empty(); }

    // JWT 토큰 생성 (쿼리 없는 요청용)
    std::string make_token() const {
        auto payload = make_payload("");
        return sign_jwt(payload);
    }

    // JWT 토큰 생성 (쿼리 파라미터 포함)
    std::string make_token(const std::string& query) const {
        auto payload = make_payload(query);
        return sign_jwt(payload);
    }

    // Authorization 헤더값
    std::string auth_header() const {
        return "Bearer " + make_token();
    }

    std::string auth_header(const std::string& query) const {
        return "Bearer " + make_token(query);
    }

private:
    std::string make_payload(const std::string& query) const {
        std::string nonce = generate_uuid();

        std::string payload = "{\"access_key\":\"" + m_access_key +
                              "\",\"nonce\":\"" + nonce + "\"";

        if (!query.empty()) {
            auto hash = sha512_hex(query);
            payload += ",\"query_hash\":\"" + hash +
                       "\",\"query_hash_alg\":\"SHA512\"";
        }

        payload += "}";
        return payload;
    }

    std::string sign_jwt(const std::string& payload) const {
        // Header: {"alg":"HS256","typ":"JWT"}
        std::string header = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";

        auto encoded_header = base64url_encode(header);
        auto encoded_payload = base64url_encode(payload);

        std::string signing_input = encoded_header + "." + encoded_payload;

        // HMAC-SHA256
        unsigned char hash[EVP_MAX_MD_SIZE];
        unsigned int len;
        HMAC(EVP_sha256(), m_secret_key.c_str(), static_cast<int>(m_secret_key.size()),
             reinterpret_cast<const unsigned char*>(signing_input.c_str()),
             signing_input.size(), hash, &len);

        auto signature = base64url_encode(std::string(reinterpret_cast<char*>(hash), len));

        return signing_input + "." + signature;
    }

    static std::string base64url_encode(const std::string& data) {
        BIO* b64 = BIO_new(BIO_f_base64());
        BIO* mem = BIO_new(BIO_s_mem());
        b64 = BIO_push(b64, mem);
        BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
        BIO_write(b64, data.c_str(), static_cast<int>(data.size()));
        BIO_flush(b64);
        BUF_MEM* bptr;
        BIO_get_mem_ptr(b64, &bptr);
        std::string result(bptr->data, bptr->length);
        BIO_free_all(b64);

        // Base64 → Base64URL
        for (auto& c : result) {
            if (c == '+') c = '-';
            else if (c == '/') c = '_';
        }
        // Remove padding '='
        while (!result.empty() && result.back() == '=') result.pop_back();

        return result;
    }

    static std::string sha512_hex(const std::string& data) {
        unsigned char hash[SHA512_DIGEST_LENGTH];
        SHA512(reinterpret_cast<const unsigned char*>(data.c_str()),
               data.size(), hash);

        std::ostringstream oss;
        for (int i = 0; i < SHA512_DIGEST_LENGTH; ++i) {
            oss << std::hex << std::setfill('0') << std::setw(2) << (int)hash[i];
        }
        return oss.str();
    }

    static std::string generate_uuid() {
        static thread_local std::mt19937 gen(
            static_cast<unsigned>(std::chrono::high_resolution_clock::now().time_since_epoch().count()));
        std::uniform_int_distribution<int> dist(0, 15);
        std::uniform_int_distribution<int> dist2(8, 11);

        const char* hex = "0123456789abcdef";
        std::string uuid = "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx";
        for (auto& c : uuid) {
            if (c == 'x') c = hex[dist(gen)];
            else if (c == 'y') c = hex[dist2(gen)];
        }
        return uuid;
    }

    std::string m_access_key;
    std::string m_secret_key;
};

} // namespace hft

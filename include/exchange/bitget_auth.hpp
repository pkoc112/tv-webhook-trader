// ============================================================================
// exchange/bitget_auth.hpp -- Bitget V2 HMAC-SHA256 인증
// v2.0 | 2026-03-13 | 서명 버퍼 재사용 최적화
// ============================================================================
#pragma once

#include <string>
#include <map>
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include "core/types.hpp"

namespace hft {

class BitgetAuth {
public:
    BitgetAuth(std::string key, std::string secret, std::string pass)
        : m_key(std::move(key)), m_secret(std::move(secret)), m_pass(std::move(pass)) {}

    std::string sign(const std::string& ts, const std::string& method,
                     const std::string& path, const std::string& body = {}) const {
        // Pre-allocate signing buffer
        m_sign_buf.clear();
        m_sign_buf.reserve(ts.size() + method.size() + path.size() + body.size());
        m_sign_buf.append(ts);
        m_sign_buf.append(method);
        m_sign_buf.append(path);
        m_sign_buf.append(body);
        return hmac_sha256_b64(m_secret, m_sign_buf);
    }

    std::map<std::string, std::string> headers(const std::string& method,
        const std::string& path, const std::string& body = {}) const {
        auto ts = std::to_string(now_ms());
        return {
            {"ACCESS-KEY",        m_key},
            {"ACCESS-SIGN",       sign(ts, method, path, body)},
            {"ACCESS-TIMESTAMP",  ts},
            {"ACCESS-PASSPHRASE", m_pass},
            {"Content-Type",      "application/json"},
            {"locale",            "en-US"}
        };
    }

private:
    std::string hmac_sha256_b64(const std::string& key, const std::string& data) const {
        unsigned char hash[EVP_MAX_MD_SIZE];
        unsigned int len;
        HMAC(EVP_sha256(), key.c_str(), static_cast<int>(key.size()),
             reinterpret_cast<const unsigned char*>(data.c_str()), data.size(), hash, &len);
        BIO* b64 = BIO_new(BIO_f_base64());
        BIO* mem = BIO_new(BIO_s_mem());
        b64 = BIO_push(b64, mem);
        BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
        BIO_write(b64, hash, static_cast<int>(len));
        BIO_flush(b64);
        BUF_MEM* bptr;
        BIO_get_mem_ptr(b64, &bptr);
        std::string result(bptr->data, bptr->length);
        BIO_free_all(b64);
        return result;
    }

    std::string m_key, m_secret, m_pass;
    mutable std::string m_sign_buf;  // 서명용 버퍼 재사용
};

} // namespace hft

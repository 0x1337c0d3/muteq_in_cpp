//
//  auth.hpp
//  muteq_server
//
//  HMAC-SHA256 signature verification matching FastAPI server.py.
//

#pragma once

#include <format>
#include <string>
#include <string_view>

#if defined(__APPLE__)
#    include <CommonCrypto/CommonHMAC.h>
#else
#    include <openssl/evp.h>
#    include <openssl/hmac.h>
#endif

namespace server {

inline std::string hmac_sha256_hex(std::string_view key, std::string_view data) {
#if defined(__APPLE__)
    unsigned char md[CC_SHA256_DIGEST_LENGTH];
    CCHmac(kCCHmacAlgSHA256, key.data(), key.size(), data.data(), data.size(), md);
    std::string out;
    out.reserve(CC_SHA256_DIGEST_LENGTH * 2);
    for (int i = 0; i < CC_SHA256_DIGEST_LENGTH; ++i) {
        out += std::format("{:02x}", md[i]);
    }
    return out;
#else
    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int md_len = 0;
    HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(data.data()), data.size(), md, &md_len);
    std::string out;
    out.reserve(md_len * 2);
    for (unsigned int i = 0; i < md_len; ++i) {
        out += std::format("{:02x}", md[i]);
    }
    return out;
#endif
}

inline bool verify_hmac(std::string_view secret, std::string_view body, std::string_view header_signature) {
    if (secret.empty())
        return true;
    const std::string expected = "sha256=" + hmac_sha256_hex(secret, body);
    return expected == header_signature;
}

} // namespace server

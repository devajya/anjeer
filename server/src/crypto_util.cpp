#include "server/crypto_util.h"

#include <iomanip>
#include <sstream>
#include <stdexcept>

#include <openssl/evp.h>
#include <openssl/rand.h>

namespace anjeer::server {

std::string sha256_hex(std::string_view input) {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int  digest_len = 0;

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) throw std::runtime_error("EVP_MD_CTX_new failed");

    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(ctx, input.data(), input.size()) != 1 ||
        EVP_DigestFinal_ex(ctx, digest, &digest_len) != 1) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("SHA-256 digest failed");
    }
    EVP_MD_CTX_free(ctx);

    std::ostringstream oss;
    for (unsigned int i = 0; i < digest_len; ++i)
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(digest[i]);
    return oss.str();
}

std::string generate_api_key() {
    constexpr int kBytes = 32;
    unsigned char buf[kBytes];
    if (RAND_bytes(buf, kBytes) != 1)
        throw std::runtime_error("RAND_bytes failed");

    std::ostringstream oss;
    oss << "ank_";
    for (int i = 0; i < kBytes; ++i)
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(buf[i]);
    return oss.str();
}

std::string generate_reconnect_token() {
    constexpr int kBytes = 32;
    unsigned char buf[kBytes];
    if (RAND_bytes(buf, kBytes) != 1)
        throw std::runtime_error("RAND_bytes failed");

    std::ostringstream oss;
    oss << "rtk_";
    for (int i = 0; i < kBytes; ++i)
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(buf[i]);
    return oss.str();
}

} // namespace anjeer::server

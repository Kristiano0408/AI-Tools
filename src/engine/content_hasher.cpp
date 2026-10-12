#include "content_hasher.h"
#include <fstream>
#include <sstream>
#include <iomanip>
#include <memory>
#include <openssl/evp.h>

namespace content_hasher {

namespace {

std::string to_hex(const unsigned char* hash, unsigned int len) {
    std::ostringstream oss;
    for (unsigned int i = 0; i < len; i++) {
        oss << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(hash[i]);
    }
    return oss.str();
}

} // namespace

std::string compute_file_hash(const std::string& file_path) {
    std::ifstream file(file_path, std::ios::binary);
    if (!file.is_open()) return "";

    auto ctx = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>(
        EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!ctx) return "";
    if (EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1) return "";

    char buffer[4096];
    // Note: the final partial block must also be hashed — read() returns false
    // on the last short read but gcount() still holds the bytes read.
    while (file.read(buffer, sizeof(buffer)) || file.gcount() > 0) {
        if (EVP_DigestUpdate(ctx.get(), buffer, static_cast<size_t>(file.gcount())) != 1) return "";
    }

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (EVP_DigestFinal_ex(ctx.get(), hash, &len) != 1) return "";

    return to_hex(hash, len);
}

std::string compute_string_hash(const std::string& content) {
    auto ctx = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>(
        EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!ctx) return "";
    if (EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) != 1) return "";
    if (EVP_DigestUpdate(ctx.get(), content.data(), content.size()) != 1) return "";

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int len = 0;
    if (EVP_DigestFinal_ex(ctx.get(), hash, &len) != 1) return "";

    return to_hex(hash, len);
}

}

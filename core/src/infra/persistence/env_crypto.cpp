#include "infra/persistence/env_crypto.hpp"

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <array>
#include <cstring>
#include <vector>

namespace core::envcrypto {
namespace {

constexpr char kPrefix[] = "enc:v1:";
constexpr std::size_t kPrefixLen = sizeof(kPrefix) - 1;
constexpr int kNonceLen = 12;
constexpr int kTagLen = 16;

std::array<unsigned char, 32> keyOf(const std::string& passphrase) {
    std::array<unsigned char, 32> k{};
    SHA256(reinterpret_cast<const unsigned char*>(passphrase.data()), passphrase.size(), k.data());
    return k;
}

std::string b64encode(const unsigned char* data, std::size_t n) {
    std::string out(4 * ((n + 2) / 3), '\0');
    int w = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(out.data()), data, static_cast<int>(n));
    out.resize(w > 0 ? static_cast<std::size_t>(w) : 0);
    return out;
}

std::vector<unsigned char> b64decode(const std::string& s) {
    if (s.empty() || s.size() % 4 != 0) return {};
    std::vector<unsigned char> out(3 * (s.size() / 4));
    int w = EVP_DecodeBlock(out.data(), reinterpret_cast<const unsigned char*>(s.data()),
                            static_cast<int>(s.size()));
    if (w < 0) return {};
    std::size_t pad = s.back() == '=' ? (s[s.size() - 2] == '=' ? 2 : 1) : 0; // '=' kept in w
    out.resize(static_cast<std::size_t>(w) - pad);
    return out;
}

} // namespace

bool isEncrypted(const std::string& v) { return v.rfind(kPrefix, 0) == 0; }

std::string encrypt(const std::string& plain, const std::string& passphrase) {
    auto key = keyOf(passphrase);
    unsigned char nonce[kNonceLen];
    if (RAND_bytes(nonce, kNonceLen) != 1) return plain;
    std::vector<unsigned char> buf(kNonceLen + plain.size() + kTagLen);
    std::memcpy(buf.data(), nonce, kNonceLen);
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return plain;
    int len = 0, fin = 0, ok = 1;
    ok &= EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr);
    ok &= EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kNonceLen, nullptr);
    ok &= EVP_EncryptInit_ex(ctx, nullptr, nullptr, key.data(), nonce);
    if (ok && !plain.empty())
        ok &= EVP_EncryptUpdate(ctx, buf.data() + kNonceLen, &len,
                                reinterpret_cast<const unsigned char*>(plain.data()),
                                static_cast<int>(plain.size()));
    ok &= EVP_EncryptFinal_ex(ctx, buf.data() + kNonceLen + len, &fin);
    ok &= EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, kTagLen, buf.data() + kNonceLen + len + fin);
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) return plain;
    return kPrefix + b64encode(buf.data(), kNonceLen + static_cast<std::size_t>(len + fin) + kTagLen);
}

std::optional<std::string> decrypt(const std::string& stored, const std::string& passphrase) {
    if (!isEncrypted(stored)) return std::nullopt;
    auto raw = b64decode(stored.substr(kPrefixLen));
    if (raw.size() < static_cast<std::size_t>(kNonceLen + kTagLen)) return std::nullopt;
    auto key = keyOf(passphrase);
    std::size_t ctLen = raw.size() - kNonceLen - kTagLen;
    std::string out(ctLen, '\0');
    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if (!ctx) return std::nullopt;
    int len = 0, fin = 0, ok = 1;
    ok &= EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, nullptr, nullptr);
    ok &= EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, kNonceLen, nullptr);
    ok &= EVP_DecryptInit_ex(ctx, nullptr, nullptr, key.data(), raw.data());
    if (ok && ctLen)
        ok &= EVP_DecryptUpdate(ctx, reinterpret_cast<unsigned char*>(out.data()), &len,
                                raw.data() + kNonceLen, static_cast<int>(ctLen));
    ok &= EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, kTagLen, raw.data() + kNonceLen + ctLen);
    ok &= EVP_DecryptFinal_ex(ctx, reinterpret_cast<unsigned char*>(out.data()) + len, &fin);
    EVP_CIPHER_CTX_free(ctx);
    if (!ok) return std::nullopt;
    out.resize(static_cast<std::size_t>(len + fin));
    return out;
}

} // namespace core::envcrypto

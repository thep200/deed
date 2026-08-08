// Format: "enc:v1:" + base64(nonce12 || ciphertext || tag16), AES-256-GCM, key = SHA-256(passphrase).
#pragma once

#include <optional>
#include <string>

namespace core::envcrypto {

bool isEncrypted(const std::string& value);
// Internal failure -> returns plain unchanged.
std::string encrypt(const std::string& plain, const std::string& passphrase);
// nullopt: not our format / bad base64 / wrong key / tampered.
std::optional<std::string> decrypt(const std::string& stored, const std::string& passphrase);

} // namespace core::envcrypto

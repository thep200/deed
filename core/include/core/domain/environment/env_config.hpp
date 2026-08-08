#pragma once

#include <string>
#include <vector>

#include "core/domain/request/request_type.hpp"

namespace core {

struct EnvKey {
  std::string key;
  std::string value;
  bool enabled = true;
  bool secret = false; // user-marked "secret" alias (flag only — persisted, no behavior yet)
};

struct Environment {
  std::string name;
  int schemaVersion = 1;
  std::vector<EnvKey> keys;
};

// Reserved base env: always merged under active env, hidden from selector, never selectable.
inline constexpr char kGlobalEnvName[] = "Global";

// Fallbacks when no .env override arrives via CoreApiClient::Config::appDefaults (the domain never reads .env).
inline constexpr int kDefaultFontSize = 11;
inline constexpr int kDefaultRamCacheSizeMb = 64;
inline constexpr int kDefaultDiskCacheSizeMb = 256;

// timeout/TLS are per-request (RequestConfig), not app-global.
struct AppConfig {
  std::string lastCollectionRoot; // most recently opened collection dir (reopened at startup)
  std::string fontName;           // display font (empty = default); from Settings
  int fontSize = kDefaultFontSize;

  // User-layer cache levels, clamped ≤ the ENV max.
  int ramCacheSizeMb = kDefaultRamCacheSizeMb;
  int diskCacheSizeMb = kDefaultDiskCacheSizeMb;
  bool cacheResponses = true;
  bool cachePersist = true;   // keep cache across restart (off -> RAM only, no disk attached)

  std::string encryptionKey; // empty = off. Applies to EVERY env — the toggle alone decides.
};

struct Session {
  int schemaVersion = 1;
  std::string lastOpenedFile; // relative
  std::string activeEnv;   // empty = no env selected ("Global" base still applies)
};

// Lazy collection tree (metadata only).
struct TreeNode {
  std::string name;    // display name (request: name field; folder: directory name)
  std::string relPath; // path relative to collection root
  bool isFolder = false;
  // present only when !isFolder:
  std::string id; // stable request id (survives rename/move)
  RequestType requestType = RequestType::Http;
  std::string methodOrType; // HTTP method, or gRPC methodType, to show as a badge
  std::vector<TreeNode> children;
};

} // namespace core

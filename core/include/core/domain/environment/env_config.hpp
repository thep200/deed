// core/domain/environment/env_config.hpp — environment, app-config, session, and the lazy collection-tree DTOs (README §7/§12).
#pragma once

#include <string>
#include <vector>

#include "core/domain/request/request_type.hpp" // TreeNode carries a RequestType (relocated; survives types.hpp removal)

namespace core {

// ---- Environment ----
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

// ---- App-global config (README §12.1) ----
// Built-in AppConfig defaults. The domain stays pure (never reads .env); the UI reads .env (FONT_SIZE,
// RAM_CACHE_SIZE, DISK_CACHE_SIZE) and passes overrides in via CoreApiClient::Config::appDefaults —
// these constants are the fallback when no .env value arrives.
inline constexpr int kDefaultFontSize = 11;
inline constexpr int kDefaultRamCacheSizeMb = 64;
inline constexpr int kDefaultDiskCacheSizeMb = 256;

// (timeout/TLS are now per-request — see RequestConfig — not app-global.)
struct AppConfig {
  std::string lastCollectionRoot; // most recently opened collection dir (reopened at startup)
  std::string fontName;           // display font (empty = default); from Settings
  int fontSize = kDefaultFontSize;

  // --- Response cache (USER layer — edited in Settings; clamped ≤ ENV max). RESPONSE_CACHE.md §1 ---
  int ramCacheSizeMb = kDefaultRamCacheSizeMb;    // operating RAM cache level
  int diskCacheSizeMb = kDefaultDiskCacheSizeMb;  // operating disk cache level
  bool cacheResponses = true; // enable/disable response cache
  bool cachePersist = true;   // keep cache across restart (off -> RAM only, no disk attached)

  // --- Env-value encryption (grid "Enc" toggle) ---
  std::string encryptionKey; // empty = off. Applies to EVERY env — the toggle alone decides.
};

// ---- Session app-state ----
struct Session {
  int schemaVersion = 1;
  std::string lastOpenedFile; // relative
  std::string activeEnv;   // empty = no env selected ("Global" base still applies)
};

// ---- Collection tree (lazy, metadata only) ----
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

// core/env_config.hpp — environment, app-config, session, and the lazy collection-tree DTOs (README §7/§12).
#pragma once

#include <string>
#include <vector>

#include "core/request_model.hpp" // TreeNode carries a RequestType

namespace core {

// ---- Environment ----
struct EnvKey {
  std::string key;
  std::string value;
  bool enabled = true;
};

struct Environment {
  std::string name;
  int schemaVersion = 1;
  std::vector<EnvKey> keys;
};

// ---- App-global config (README §12.1) ----
// (timeout/TLS are now per-request — see RequestConfig — not app-global.)
struct AppConfig {
  std::string lastCollectionRoot; // most recently opened collection dir (reopened at startup)
  std::string fontName;           // display font (empty = default); from Settings
  int fontSize = 11;

  // --- Response cache (USER layer — edited in Settings; clamped ≤ ENV max). RESPONSE_CACHE.md §1 ---
  int ramCacheSizeMb = 64;    // operating RAM cache level
  int diskCacheSizeMb = 256;  // operating disk cache level
  bool cacheResponses = true; // enable/disable response cache
  bool cachePersist = true;   // keep cache across restart (off -> RAM only, no disk attached)
};

// ---- Session app-state ----
struct Session {
  int schemaVersion = 1;
  std::string lastOpenedFile; // relative
  std::string activeEnv;   // empty = no env selected (no special base; all envs are equal)
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

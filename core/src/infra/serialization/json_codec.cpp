#include "infra/serialization/json_codec.hpp"

#include <stdexcept>
#include <string>

namespace core::codec {

json parseGuarded(const std::string& text, int maxDepth) {
    // O(n) depth pre-scan outside string literals: reject before the recursive parser can blow the stack.
    int depth = 0;
    bool inStr = false, esc = false;
    for (char c : text) {
        if (inStr) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') inStr = false;
            continue;
        }
        if (c == '"') inStr = true;
        else if (c == '[' || c == '{') {
            // runtime_error is caught by the same catch(const std::exception&) sites as json::parse_error.
            if (++depth > maxDepth)
                throw std::runtime_error("JSON nesting too deep (max " + std::to_string(maxDepth) + ")");
        } else if (c == ']' || c == '}') {
            if (depth > 0) --depth;
        }
    }
    return json::parse(text);
}

json toJson(const Environment& e) {
    json keys = json::array();
    for (const auto& k : e.keys) {
        keys.push_back(json{{"key", k.key}, {"value", k.value}, {"enabled", k.enabled ? 1 : 0},
                            {"secret", k.secret ? 1 : 0}});
    }
    // No "name" field: the env name is the FILENAME (EnvironmentStore sets it on load). Don't duplicate it.
    return json{{"schemaVersion", e.schemaVersion}, {"keys", keys}};
}

Environment envFromJson(const json& j) {
    Environment e;
    e.schemaVersion = getInt(j, "schemaVersion", 1);
    e.name = getStr(j, "name");
    if (auto it = j.find("keys"); it != j.end() && it->is_array()) {
        for (const auto& k : *it) {
            EnvKey ek;
            ek.key = getStr(k, "key");
            ek.value = getStr(k, "value");   // "enc:v1:..." when encrypted at rest (env_crypto)
            ek.enabled = getBool(k, "enabled", true);
            ek.secret = getBool(k, "secret", false);
            e.keys.push_back(ek);
        }
    }
    return e;
}

// snake_case keys (app config). cacheResponses/cachePersist NOT exposed to user -> always default true.
json toJson(const AppConfig& c) {
    return json{{"last_collection_root", c.lastCollectionRoot},
                {"font_name", c.fontName},
                {"font_size", c.fontSize},
                {"ram_cache_size", c.ramCacheSizeMb},
                {"disk_cache_size", c.diskCacheSizeMb},
                {"encryption_key", c.encryptionKey}};
}
AppConfig appConfigFromJson(const json& j) { return appConfigFromJson(j, AppConfig{}); }

AppConfig appConfigFromJson(const json& j, const AppConfig& def) {
    AppConfig c = def;   // missing key -> keep the default value (from .env)
    if (j.find("last_collection_root") != j.end()) c.lastCollectionRoot = getStr(j, "last_collection_root");
    if (j.find("font_name") != j.end()) c.fontName = getStr(j, "font_name");
    c.fontSize = getInt(j, "font_size", def.fontSize);
    c.ramCacheSizeMb = getInt(j, "ram_cache_size", def.ramCacheSizeMb);
    c.diskCacheSizeMb = getInt(j, "disk_cache_size", def.diskCacheSizeMb);
    if (j.find("encryption_key") != j.end()) c.encryptionKey = getStr(j, "encryption_key");
    // cacheResponses/cachePersist are not user config -> the struct's default (true) stands.
    return c;
}

json toJson(const Session& s) {
    return json{{"schemaVersion", s.schemaVersion},
                {"lastOpenedFile", s.lastOpenedFile},
                {"activeEnv", s.activeEnv}};
}
Session sessionFromJson(const json& j) {
    Session s;
    s.schemaVersion = getInt(j, "schemaVersion", 1);
    s.lastOpenedFile = getStr(j, "lastOpenedFile");
    s.activeEnv = getStr(j, "activeEnv", "");   // empty = no env selected (no special base)
    return s;
}

} // namespace core::codec

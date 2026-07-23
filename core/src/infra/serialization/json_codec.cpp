#include "infra/serialization/json_codec.hpp"

#include <stdexcept>
#include <string>

namespace core::codec {

json parseGuarded(const std::string& text, int maxDepth) {
    // O(n) structural-depth pre-scan: count [ and { nesting OUTSIDE of string literals. Reject before the
    // recursive parser can blow the stack (H5). String/escape handling so brackets inside strings don't count.
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
            // std::runtime_error derives from std::exception, same as json::parse_error — every existing
            // catch(const std::exception&)/catch(...) around these parse sites handles it identically.
            if (++depth > maxDepth)
                throw std::runtime_error("JSON nesting too deep (max " + std::to_string(maxDepth) + ")");
        } else if (c == ']' || c == '}') {
            if (depth > 0) --depth;
        }
    }
    return json::parse(text);
}

namespace {

// Read int by snake_case key, fall back to the old camelCase key (compat with old config.json).
int getIntCompat(const json& j, const char* snake, const char* camel, int def) {
    if (j.find(snake) != j.end()) return getInt(j, snake, def);
    return getInt(j, camel, def);
}
std::string getStrCompat(const json& j, const char* snake, const char* camel,
                         const std::string& def = "") {
    if (j.find(snake) != j.end()) return getStr(j, snake);
    if (j.find(camel) != j.end()) return getStr(j, camel);
    return def;
}

} // namespace

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
            ek.value = getStr(k, "value");   // old env files may lack value if it was once a secret;
                                             // migrateLegacySecrets() already merged the value back in earlier.
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
                {"theme", c.theme},
                {"ram_cache_size", c.ramCacheSizeMb},
                {"disk_cache_size", c.diskCacheSizeMb}};
}
AppConfig appConfigFromJson(const json& j) { return appConfigFromJson(j, AppConfig{}); }

AppConfig appConfigFromJson(const json& j, const AppConfig& def) {
    AppConfig c = def;   // missing key -> keep the default value (from .env)
    c.lastCollectionRoot = getStrCompat(j, "last_collection_root", "lastCollectionRoot", def.lastCollectionRoot);
    c.fontName = getStrCompat(j, "font_name", "fontName", def.fontName);
    c.fontSize = getIntCompat(j, "font_size", "fontSize", def.fontSize);
    c.theme = getStrCompat(j, "theme", "theme", def.theme);
    c.ramCacheSizeMb = getIntCompat(j, "ram_cache_size", "ramCacheSizeMb", def.ramCacheSizeMb);
    c.diskCacheSizeMb = getIntCompat(j, "disk_cache_size", "diskCacheSizeMb", def.diskCacheSizeMb);
    // cacheResponses/cachePersist no longer in user config -> keep the struct's default true.
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

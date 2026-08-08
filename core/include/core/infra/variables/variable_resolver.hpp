#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace core {

struct ResolveResult {
    std::string text;
    std::vector<std::string> missing; // {{X}} vars that don't exist (kept literal + warning)
};

class VariableResolver {
public:
    // Replace {{X}} with vars[X]. X exists but empty -> ""; X missing -> keep "{{X}}".
    static ResolveResult resolve(const std::string& tpl,
                                 const std::map<std::string, std::string>& vars);

    // Reverse substitution: rewrite a literal value back to {{alias}}.
    // `vars` order: active env pairs first, then non-shadowed Global; on duplicate values the FIRST entry wins.

    // Whole-value match: `value` exactly equals some non-empty vars value -> out = "{{key}}", returns true; else false.
    static bool valueToAlias(const std::string& value,
                             const std::vector<std::pair<std::string, std::string>>& vars,
                             std::string& out, std::string* key = nullptr);

    // Prefix match (vars value length >= kMinPrefixLen): replaces the prefix with "{{key}}", keeping the remainder.
    // Longest value wins; ties -> the first-defined key.
    static bool prefixToAlias(const std::string& value,
                              const std::vector<std::pair<std::string, std::string>>& vars,
                              std::string& out, std::string* key = nullptr);

    // Floor for prefix matching only (avoids a 1-2 char value mangling unrelated text); whole-value matching has none.
    static constexpr std::size_t kMinPrefixLen = 4;
};

} // namespace core

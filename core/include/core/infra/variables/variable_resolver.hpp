// core/variable_resolver.hpp — Resolve {{var}} (README §9.5). Pure function, easy to test.
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

    // --- Reverse substitution: rewrite a literal value back to {{alias}} (README §9.5). ---
    // Used to proactively replace hardcoded values that match the active env with an alias.

    // `vars` order: active env pairs first, then non-shadowed Global pairs (mergedVars). On duplicate
    // values the FIRST entry wins.

    // Whole-value match: if `value` exactly equals some vars[key].value (non-empty), set `out` to
    // "{{key}}" and return true. For headers/query/auth/metadata, where the field holds the bare
    // value. No change (returns false) otherwise.
    static bool valueToAlias(const std::string& value,
                             const std::vector<std::pair<std::string, std::string>>& vars,
                             std::string& out, std::string* key = nullptr);

    // Prefix match: if some vars[key].value (length >= kMinPrefixLen) is a prefix of `value`,
    // replace that prefix with "{{key}}" (keeping the remainder) and return true. Longest value
    // wins; ties -> the first-defined key. For url / gRPC target, where the alias is usually a
    // baseUrl prefix. The length floor avoids a 1-2 char value mangling unrelated text.
    static bool prefixToAlias(const std::string& value,
                              const std::vector<std::pair<std::string, std::string>>& vars,
                              std::string& out, std::string* key = nullptr);

    // Minimum env-value length eligible for prefix matching (url/target). Whole-value matching
    // has no floor — exact equality is unambiguous even for short values.
    static constexpr std::size_t kMinPrefixLen = 4;
};

} // namespace core

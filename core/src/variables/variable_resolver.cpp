#include "core/variables/variable_resolver.hpp"

namespace core {

ResolveResult VariableResolver::resolve(const std::string& tpl,
                                        const std::map<std::string, std::string>& vars) {
    ResolveResult r;
    r.text.reserve(tpl.size());
    size_t i = 0;
    const size_t n = tpl.size();
    while (i < n) {
        // find "{{"
        if (i + 1 < n && tpl[i] == '{' && tpl[i + 1] == '{') {
            size_t close = tpl.find("}}", i + 2);
            if (close != std::string::npos) {
                std::string name = tpl.substr(i + 2, close - (i + 2));
                // trim whitespace around the name
                size_t a = name.find_first_not_of(" \t");
                size_t b = name.find_last_not_of(" \t");
                std::string key = (a == std::string::npos) ? "" : name.substr(a, b - a + 1);
                auto it = vars.find(key);
                if (it != vars.end()) {
                    r.text += it->second;           // exists (including empty -> "")
                } else {
                    r.text += tpl.substr(i, close + 2 - i); // keep literal "{{X}}"
                    r.missing.push_back(key);
                }
                i = close + 2;
                continue;
            }
        }
        r.text += tpl[i++];
    }
    return r;
}

bool VariableResolver::valueToAlias(const std::string& value,
                                    const std::vector<std::pair<std::string, std::string>>& vars,
                                    std::string& out, std::string* key) {
    if (value.empty()) return false;
    for (const auto& [k, v] : vars) {       // env definition order -> first matching key wins
        if (!v.empty() && v == value) {
            out = "{{" + k + "}}";
            if (key) *key = k;
            return true;
        }
    }
    return false;
}

bool VariableResolver::prefixToAlias(const std::string& value,
                                     const std::vector<std::pair<std::string, std::string>>& vars,
                                     std::string& out, std::string* key) {
    if (value.empty()) return false;
    const std::string* bestKey = nullptr;
    std::size_t bestLen = 0;
    for (const auto& [k, v] : vars) {
        // longest value wins; equal length -> keep the earlier (first-defined) one.
        if (v.size() < kMinPrefixLen || v.size() <= bestLen) continue;
        if (value.compare(0, v.size(), v) != 0) continue;
        bestKey = &k; bestLen = v.size();
    }
    if (!bestKey) return false;
    out = "{{" + *bestKey + "}}" + value.substr(bestLen);
    if (key) *key = *bestKey;
    return true;
}

} // namespace core

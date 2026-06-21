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

} // namespace core

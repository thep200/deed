// core/variable_resolver.hpp — Resolve {{var}} (README §9.5). Pure function, easy to test.
#pragma once

#include <map>
#include <string>
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
};

} // namespace core

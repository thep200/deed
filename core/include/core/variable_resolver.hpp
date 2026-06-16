// core/variable_resolver.hpp — Resolve {{var}} (README §9.5). Hàm thuần, dễ test.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace core {

struct ResolveResult {
    std::string text;
    std::vector<std::string> missing; // các biến {{X}} không tồn tại (giữ literal + warning)
};

class VariableResolver {
public:
    // Thay {{X}} bằng vars[X]. X tồn tại nhưng rỗng -> ""; X không tồn tại -> giữ "{{X}}".
    static ResolveResult resolve(const std::string& tpl,
                                 const std::map<std::string, std::string>& vars);
};

} // namespace core

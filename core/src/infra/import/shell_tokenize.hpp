// Minimal tokenizer: single/double quotes, '\' escape & line continuation, $'...'. Not a full shell.
#pragma once

#include <string>
#include <vector>

namespace core {

std::vector<std::string> shellTokenize(const std::string& input);

std::string trim(const std::string& s);
std::string lower(std::string s);

} // namespace core

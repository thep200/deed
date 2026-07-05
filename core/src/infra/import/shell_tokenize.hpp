// shell_tokenize.hpp — minimal shell command-line tokenizer for import (README §8.2).
// Handles: single/double quotes, '\' escape & line continuation, $'...'. Not a full shell.
#pragma once

#include <string>
#include <vector>

namespace core {

std::vector<std::string> shellTokenize(const std::string& input);

// Small string helpers shared by the importers.
std::string trim(const std::string& s);
std::string lower(std::string s);

} // namespace core

// shell_tokenize.hpp — minimal shell command-line tokenizer for import (README §8.2).
// Handles: single/double quotes, '\' escape & line continuation, $'...'. Not a full shell.
#pragma once

#include <string>
#include <vector>

namespace core {

std::vector<std::string> shellTokenize(const std::string& input);

} // namespace core

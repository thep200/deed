// shell_tokenize.hpp — tokenizer dòng lệnh shell tối giản cho import (README §8.2).
// Xử lý: nháy đơn/kép, '\' escape & nối dòng, $'...'. Không cố bao trọn shell.
#pragma once

#include <string>
#include <vector>

namespace core {

std::vector<std::string> shellTokenize(const std::string& input);

} // namespace core

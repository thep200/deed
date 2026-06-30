// core/domain/ports/driving/exec_id.hpp — handle identifying one in-flight request execution (REFACTOR_SPEC §6.1).
#pragma once

#include "core/domain/common/strong_string.hpp"

namespace core::domain {

using RequestExecutionId = StrongString<struct ExecTag>;

} // namespace core::domain

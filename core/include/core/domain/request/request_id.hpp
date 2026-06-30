// core/domain/request/request_id.hpp — RequestId identity (REFACTOR_SPEC §5.8).
#pragma once

#include "core/domain/common/strong_string.hpp"

namespace core::domain {

struct RequestIdTag {};
using RequestId = StrongString<RequestIdTag>;

} // namespace core::domain

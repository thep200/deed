#pragma once

#include "core/domain/common/strong_string.hpp"

namespace core::domain {

struct RequestIdTag {};
using RequestId = StrongString<RequestIdTag>;

} // namespace core::domain

#pragma once

#include <string>

#include "core/domain/request/request_model.hpp"

namespace core {

// Renders a cURL (HTTP) / grpcurl (gRPC) command. Pass a RESOLVED model.
std::string toCurl(const core::domain::RequestModel& resolved);

} // namespace core

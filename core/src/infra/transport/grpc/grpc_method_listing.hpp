// Lib-free facade over grpc_descriptors (domain types only) — safe to include from the app layer.
#pragma once

#include <string>
#include <vector>

#include "core/domain/grpc/grpc_method.hpp"
#include "core/domain/grpc/grpc_request.hpp"

namespace core::grpcdesc {

// `g` must already be {{var}}-resolved. On failure returns {} and sets err.
std::vector<core::domain::GrpcMethodDescriptor> listGrpcMethods(const core::domain::GrpcRequest& g,
                                                                std::string& err);

} // namespace core::grpcdesc

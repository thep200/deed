// grpc_method_listing.hpp — lib-free facade over grpc_descriptors (domain types only, no protobuf/grpc
// includes), safe to include from the app layer (composition_root's listGrpcMethods).
#pragma once

#include <string>
#include <vector>

#include "core/domain/grpc/grpc_method.hpp"  // domain GrpcMethodDescriptor
#include "core/domain/grpc/grpc_request.hpp" // domain GrpcRequest payload

namespace core::grpcdesc {

// Build + list descriptors for an already-resolved domain GrpcRequest (reflection / proto files / FDS).
// On failure returns {} and sets err.
std::vector<core::domain::GrpcMethodDescriptor> listGrpcMethods(const core::domain::GrpcRequest& g,
                                                                std::string& err);

} // namespace core::grpcdesc

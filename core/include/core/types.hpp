// core/types.hpp — Core's neutral DTOs (README §7). FAÇADE: the structs now live in bounded-context
// headers (split per SPEC_refactor §4.1); this header re-exports them so existing `#include "core/types.hpp"`
// keeps working. UI only sees these structs; do NOT leak grpc++/protobuf/libcurl/nlohmann out the port.
//
// New code may include the narrower header directly:
//   request_model.hpp  — RequestType, Http/Grpc/Ws/GraphQl blocks, RequestConfig, RequestModel
//   streaming_dto.hpp  — Stream* DTOs, InteractionKind, ResolvedRequest
//   response.hpp       — ApiResponse, ApiError, ValidationResult, RequestHandle, Progress
//   env_config.hpp     — Environment, AppConfig, Session, TreeNode
//   dto_common.hpp     — KeyValue, MultipartPart
#pragma once

#include "core/dto_common.hpp"
#include "core/env_config.hpp"
#include "core/request_model.hpp"
#include "core/response.hpp"
#include "core/streaming_dto.hpp"

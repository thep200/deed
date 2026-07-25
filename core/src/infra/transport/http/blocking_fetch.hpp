// blocking_fetch.hpp — one-shot out-of-band HTTP call on the calling thread. INTERNAL (core/src).
// Wraps the CaptureSink + local-NativeHttpSender pattern (gql_introspection / oauth2 precedent): the
// LOCAL sender means the shared senders' active cancel token is never touched (no cross-cancel).
#pragma once

#include "core/domain/common/result.hpp"
#include "core/domain/ports/driven/i_cancellation_token.hpp"
#include "core/domain/request/request_model.hpp"
#include "core/domain/response/api_response.hpp"

namespace core::infra {

// Executes `model` (must be an Http payload) and returns the terminal response. EvFailed or no
// terminal event -> Result::fail(Network). HTTP status is NOT interpreted — callers decide what >=400 means.
core::domain::Result<core::domain::ApiResponse>
blockingFetch(const core::domain::RequestModel &model, const core::domain::ICancellationToken &cancel);

} // namespace core::infra

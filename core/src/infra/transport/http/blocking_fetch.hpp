// One-shot out-of-band HTTP call on the calling thread; a LOCAL sender so the shared senders' cancel state is never touched.
#pragma once

#include "core/domain/common/result.hpp"
#include "core/domain/ports/driven/i_cancellation_token.hpp"
#include "core/domain/request/request_model.hpp"
#include "core/domain/response/api_response.hpp"

namespace core::infra {

// `model` must carry an Http payload. EvFailed / no terminal -> fail(Network); HTTP status is NOT interpreted.
core::domain::Result<core::domain::ApiResponse>
blockingFetch(const core::domain::RequestModel &model, const core::domain::ICancellationToken &cancel);

} // namespace core::infra

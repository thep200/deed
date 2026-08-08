#pragma once

#include "infra/transport/typed_sender.hpp"

namespace core::infra {

// Stateless across calls: cancel hangs off the caller's token (linkCancel), never a member slot — one
// instance serves every concurrent send. No close() override for the same reason.
class NativeHttpSender final : public TypedSender<domain::HttpRequest> {
protected:
  domain::Status executeTyped(const domain::RequestModel &resolved, const domain::HttpRequest &http,
                              domain::IResponseSink &sink,
                              const domain::ICancellationToken &cancel) override;
};

} // namespace core::infra

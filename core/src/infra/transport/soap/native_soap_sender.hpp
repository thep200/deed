// SOAP adds no transport of its own: the envelope POSTs through an owned NativeHttpSender.
#pragma once

#include <string>

#include "core/domain/request/request_model.hpp"
#include "infra/transport/http/native_http_sender.hpp"
#include "infra/transport/typed_sender.hpp"

namespace core::infra {

namespace soap {
// body = envelope (raw XML); version headers are added only when the user hasn't set their own.
core::domain::RequestModel buildSoapHttpModel(const core::domain::RequestModel &model);
// Best-effort: <faultstring> (1.1) or the first <…:Text> inside a Fault (1.2); empty when unrecognizable.
std::string extractFaultString(const std::string &body);
} // namespace soap

class NativeSoapSender final : public TypedSender<domain::SoapRequest> {
protected:
  // Cancel rides the token straight through to the inner HTTP POST — no close() routing needed.
  domain::Status executeTyped(const domain::RequestModel &resolved, const domain::SoapRequest &soap,
                              domain::IResponseSink &sink,
                              const domain::ICancellationToken &cancel) override;

private:
  NativeHttpSender http_;
};

} // namespace core::infra

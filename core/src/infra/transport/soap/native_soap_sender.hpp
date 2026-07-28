// native_soap_sender.hpp — SOAP over HTTP (SPEC_soap §4). INTERNAL (core/src). SOAP adds NO transport:
// the envelope POSTs through an owned NativeHttpSender (mirrors NativeGraphQlSender's shape).
#pragma once

#include <string>

#include "core/domain/ports/driven/i_request_sender.hpp"
#include "core/domain/request/request_model.hpp"
#include "infra/transport/http/native_http_sender.hpp"

namespace core::infra {

namespace soap {
// Pure: package the SOAP request as an HTTP POST — body = envelope (raw XML), version headers added
// only when the user hasn't set their own (SPEC_soap §4). Unit-tested.
core::domain::RequestModel buildSoapHttpModel(const core::domain::RequestModel &model);
// Pure: best-effort faultstring out of a response body — <faultstring>…</faultstring> (1.1) or the
// first <…:Text>…</…:Text> inside a Fault (1.2). Empty when nothing recognizable. No XML parser.
std::string extractFaultString(const std::string &body);
} // namespace soap

class NativeSoapSender final : public domain::IRequestSender {
public:
  bool supports(domain::RequestType t) const override { return t == domain::RequestType::Soap; }
  domain::Status execute(const domain::RequestModel &resolved, domain::IResponseSink &sink,
                         const domain::ICancellationToken &cancel) override;
  domain::Status close(int code, std::string reason) override { return http_.close(code, std::move(reason)); }

private:
  NativeHttpSender http_;
};

} // namespace core::infra

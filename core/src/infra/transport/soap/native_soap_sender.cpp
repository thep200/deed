// native_soap_sender.cpp — SOAP -> HTTP packaging + version header policy (SPEC_soap §4).
#include "infra/transport/soap/native_soap_sender.hpp"

#include <cctype>
#include <vector>

#include "core/domain/body/body.hpp"
#include "core/domain/http/http_request.hpp"
#include "core/domain/response/response_event.hpp"

namespace core::infra {
namespace d = core::domain;

namespace soap {
namespace {

bool hasHeader(const d::HeaderList &headers, const char *lowerName) {
  for (const auto &h : headers.items()) {
    if (!h.enabled()) continue;
    std::string k = h.name();
    for (auto &c : k) c = (char)std::tolower((unsigned char)c);
    if (k == lowerName) return true;
  }
  return false;
}

} // namespace

d::RequestModel buildSoapHttpModel(const d::RequestModel &model) {
  const d::SoapRequest &s = std::get<d::SoapRequest>(model.payload());

  std::vector<d::Header> hdrs = s.headers().items();
  if (!hasHeader(s.headers(), "content-type")) {
    if (s.version() == d::SoapVersion::V1_1) {
      hdrs.push_back(d::Header::create("Content-Type", "text/xml; charset=utf-8").take());
    } else {
      std::string ct = "application/soap+xml; charset=utf-8";
      if (!s.action().empty()) ct += "; action=\"" + s.action() + "\"";
      hdrs.push_back(d::Header::create("Content-Type", ct).take());
    }
  }
  // 1.1 REQUIRES the SOAPAction header on every call (empty action -> SOAPAction: ""). A user-set
  // SOAPAction in the Headers tab wins. 1.2 has no such header (action rides in the Content-Type).
  if (s.version() == d::SoapVersion::V1_1 && !hasHeader(s.headers(), "soapaction"))
    hdrs.push_back(d::Header::create("SOAPAction", "\"" + s.action() + "\"").take());

  d::HttpRequest::Parts hp{d::HttpMethod::Post,
                           s.url(),
                           {},
                           {},
                           d::HeaderList(std::move(hdrs)),
                           d::Body::raw(d::RawSubtype::Xml, s.envelope()),
                           s.auth()};
  auto http = d::HttpRequest::create(std::move(hp)).take();
  return d::RequestModel::create(model.id(), model.name(), model.seq(), model.config(), http).take();
}

std::string extractFaultString(const std::string &body) {
  // 1.1: <faultstring ...>text</faultstring>
  if (auto open = body.find("<faultstring"); open != std::string::npos) {
    auto gt = body.find('>', open);
    auto close = body.find("</faultstring>", open);
    if (gt != std::string::npos && close != std::string::npos && gt + 1 <= close)
      return body.substr(gt + 1, close - gt - 1);
  }
  // 1.2: first <ns:Text ...>text</ns:Text> (inside <ns:Reason>); accept any prefix, conservative.
  if (auto reason = body.find("Reason>"); reason != std::string::npos) {
    auto open = body.find(":Text", reason);
    if (open == std::string::npos) return {};
    auto gt = body.find('>', open);
    if (gt == std::string::npos) return {};
    auto lt = body.find("</", gt);
    if (lt == std::string::npos) return {};
    return body.substr(gt + 1, lt - gt - 1);
  }
  return {};
}

} // namespace soap

d::Status NativeSoapSender::execute(const d::RequestModel &resolved, d::IResponseSink &sink,
                                    const d::ICancellationToken &cancel) {
  const auto &s = std::get<d::SoapRequest>(resolved.payload());
  if (s.url().raw().empty()) {
    sink.emit(d::ResponseEvent(
        d::EvFailed{{d::ErrorKind::Protocol, "soap endpoint url required", {}}}));
    return d::ok();
  }
  if (s.envelope().find_first_not_of(" \t\r\n") == std::string::npos) {
    sink.emit(d::ResponseEvent(
        d::EvFailed{{d::ErrorKind::Protocol, "soap envelope is empty", {}}}));
    return d::ok();
  }
  return http_.execute(soap::buildSoapHttpModel(resolved), sink, cancel);
}

} // namespace core::infra

// core/domain/soap/soap_request.hpp — SOAP aggregate payload (SPEC_soap §3).
// SOAP adds NO transport: the sender packages this into an HTTP POST (envelope = the body, verbatim).
// The domain never parses XML — the envelope is opaque text the user edits in full (SPEC_soap §1).
#pragma once

#include <string>
#include <utility>

#include "core/domain/auth/auth.hpp"
#include "core/domain/common/result.hpp"
#include "core/domain/values/header.hpp"
#include "core/domain/values/url.hpp"

namespace core::domain {

// Protocol dialect — decides the Content-Type and where the action rides (SPEC_soap §4):
// 1.1 = text/xml + a mandatory SOAPAction header; 1.2 = application/soap+xml with an action param.
enum class SoapVersion { V1_1, V1_2 };

class SoapRequest {
public:
  struct Parts {
    Url url;                                  // endpoint; empty OK on a draft (send-time concern)
    std::string action;                       // SOAPAction / action param; empty is valid
    SoapVersion version = SoapVersion::V1_1;
    std::string envelope;                     // full XML envelope, verbatim
    HeaderList headers;
    Auth auth = Auth::none();
  };

  // Lenient like Http/GraphQL drafts: emptiness is checked at send time by the sender.
  static Result<SoapRequest> create(Parts p) {
    return Result<SoapRequest>::ok(SoapRequest(std::move(p)));
  }

  const Url &url() const noexcept { return p_.url; }
  const std::string &action() const noexcept { return p_.action; }
  SoapVersion version() const noexcept { return p_.version; }
  const std::string &envelope() const noexcept { return p_.envelope; }
  const HeaderList &headers() const noexcept { return p_.headers; }
  const Auth &auth() const noexcept { return p_.auth; }

  bool operator==(const SoapRequest &o) const {
    return p_.url == o.p_.url && p_.action == o.p_.action && p_.version == o.p_.version &&
           p_.envelope == o.p_.envelope && p_.headers == o.p_.headers && p_.auth == o.p_.auth;
  }
  bool operator!=(const SoapRequest &o) const { return !(*this == o); }

private:
  explicit SoapRequest(Parts p) : p_(std::move(p)) {}
  Parts p_;
};

} // namespace core::domain

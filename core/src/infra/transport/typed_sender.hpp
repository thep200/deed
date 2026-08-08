#pragma once

#include <variant>

#include "core/domain/ports/driven/i_request_sender.hpp"
#include "core/domain/request/request_traits.hpp"
#include "core/domain/response/response_event.hpp"

namespace core::infra {

// supports() derives from RequestTraits; execute() narrows the payload for executeTyped(). The mismatch
// branch is unreachable after the saga's supports() scan; emit-and-ok keeps exactly one terminal if it fires anyway.
template <class P> class TypedSender : public domain::IRequestSender {
public:
  bool supports(domain::RequestType t) const final {
    return t == domain::RequestTraits<P>::type;
  }

  domain::Status execute(const domain::RequestModel &resolved, domain::IResponseSink &sink,
                         const domain::ICancellationToken &cancel) final {
    const P *payload = std::get_if<P>(&resolved.payload());
    if (!payload) {
      sink.emit(domain::ResponseEvent(
          domain::EvFailed{{domain::ErrorKind::Unsupported, mismatchMessage(), {}}}));
      return domain::ok();
    }
    return executeTyped(resolved, *payload, sink, cancel);
  }

protected:
  virtual domain::Status executeTyped(const domain::RequestModel &resolved, const P &payload,
                                      domain::IResponseSink &sink,
                                      const domain::ICancellationToken &cancel) = 0;
  virtual const char *mismatchMessage() const { return "payload does not match sender type"; }
};

} // namespace core::infra

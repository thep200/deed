#include "infra/transport/http/blocking_fetch.hpp"

#include <optional>

#include "core/domain/ports/driven/i_response_sink.hpp"
#include "core/domain/response/response_event.hpp"
#include "infra/transport/http/native_http_sender.hpp"

namespace core::infra {
namespace d = core::domain;

d::Result<d::ApiResponse> blockingFetch(const d::RequestModel &model,
                                        const d::ICancellationToken &cancel) {
  struct CaptureSink final : d::IResponseSink {
    std::optional<d::ApiResponse> ok;
    std::optional<d::ApiError> err;
    void emit(const d::ResponseEvent &ev) override {
      if (const auto *c = ev.get<d::EvCompleted>()) ok = c->summary;
      else if (const auto *f = ev.get<d::EvFailed>()) err = f->error;
    }
  } sink;

  NativeHttpSender http;
  d::Status st = http.execute(model, sink, cancel);
  if (sink.err) return d::Result<d::ApiResponse>::fail({d::ErrorCode::Network, sink.err->message, ""});
  if (!sink.ok)
    return d::Result<d::ApiResponse>::fail(
        {d::ErrorCode::Network, st ? std::string("no response") : st.error().message, ""});
  return d::Result<d::ApiResponse>::ok(std::move(*sink.ok));
}

} // namespace core::infra

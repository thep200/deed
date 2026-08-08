#include "core/app/send_request_saga.hpp"

#include "core/domain/auth/with_auth.hpp"

#include <chrono>
#include <functional>
#include <optional>
#include <utility>

namespace core::app {
namespace d = core::domain;

namespace {

d::ErrorKind toErrorKind(d::ErrorCode c) {
  switch (c) {
  case d::ErrorCode::Cancelled: return d::ErrorKind::Cancelled;
  case d::ErrorCode::Timeout: return d::ErrorKind::Timeout;
  case d::ErrorCode::Network: return d::ErrorKind::Network;
  case d::ErrorCode::Tls: return d::ErrorKind::Tls;
  case d::ErrorCode::Parse: return d::ErrorKind::Parse;
  case d::ErrorCode::Unsupported: return d::ErrorKind::Unsupported;
  default: return d::ErrorKind::Internal;
  }
}

// One overload per JSON-carrying type; the template fallback is the deliberate "nothing to validate" default.
std::optional<d::JsonText> validatableJson(const d::HttpRequest &p) {
  std::optional<d::JsonText> out;
  p.body().match([&](auto &&b) {
    using B = std::decay_t<decltype(b)>;
    if constexpr (std::is_same_v<B, d::BodyRaw>)
      if (b.subtype == d::RawSubtype::Json) out = d::JsonText::of(b.text);
  });
  return out;
}
std::optional<d::JsonText> validatableJson(const d::GrpcRequest &p) {
  if (!p.message().empty()) return p.message();
  return std::nullopt;
}
std::optional<d::JsonText> validatableJson(const d::KafkaRequest &p) {
  // Producer value is always JSON; an empty (draft) value skips validation.
  std::optional<d::JsonText> out;
  p.match([&](auto &&spec) {
    using S = std::decay_t<decltype(spec)>;
    if constexpr (std::is_same_v<S, d::KafkaProduceSpec>) {
      if (!spec.message.value.value.empty()) out = d::JsonText::of(spec.message.value.value);
    }
  });
  return out;
}
template <class T> std::optional<d::JsonText> validatableJson(const T &) { return std::nullopt; }

std::optional<d::JsonText> jsonToValidate(const d::RequestModel &m) {
  return m.match([](const auto &p) { return validatableJson(p); });
}

struct FnSink final : d::IResponseSink {
  std::function<void(const d::ResponseEvent &)> fn;
  void emit(const d::ResponseEvent &ev) override { fn(ev); }
};

} // namespace

SendRequestSaga::SendRequestSaga(d::RequestExecutionId exec, d::RequestModel request, Deps deps)
    : exec_(std::move(exec)), request_(std::move(request)), deps_(std::move(deps)) {}

void SendRequestSaga::run(d::IRequestObserver &observer) {
  // Single emit path so cancel/validate paths and sender-driven events share state + cache side effects.
  auto emit = [&](const d::ResponseEvent &ev) {
    if (const auto *c = ev.get<d::EvCompleted>()) {
      if (deps_.cache) deps_.cache->put(request_.id(), c->summary);
      state_ = SagaState::Completed;
    } else if (const auto *f = ev.get<d::EvFailed>()) {
      state_ = f->error.kind == d::ErrorKind::Cancelled ? SagaState::Cancelled : SagaState::Failed;
    } else if (ev.is<d::EvClosed>()) {
      state_ = token_.cancelled() ? SagaState::Cancelled : SagaState::Completed;
    } else if (ev.is<d::EvMessage>() || ev.is<d::EvKafkaRecord>()) {
      state_ = SagaState::Streaming;
    }
    observer.onEvent(exec_, ev);
  };

  if (token_.cancelled()) {
    emit(d::ResponseEvent(d::EvFailed{{d::ErrorKind::Cancelled, "cancelled", {}}}));
    return;
  }

  state_ = SagaState::Validating;
  if (deps_.jsonValidator) {
    if (auto jt = jsonToValidate(request_)) {
      auto v = deps_.jsonValidator->validate(*jt);
      if (!v) {
        emit(d::ResponseEvent(d::EvFailed{{d::ErrorKind::Parse, v.error().message, {}}}));
        return;
      }
    }
  }

  // Senders only ever see none/basic/bearer; the token POST blocks this worker bounded by the
  // request's own timeout, and Cancel aborts it via token_.
  state_ = SagaState::Preparing;
  if (const auto *oauth = d::oauth2Of(request_)) {
    if (!deps_.tokenProvider) {
      emit(d::ResponseEvent(d::EvFailed{{d::ErrorKind::Unsupported, "oauth2 not configured", {}}}));
      return;
    }
    auto tok = deps_.tokenProvider->bearerFor(*oauth, request_.config().timeout, token_);
    auto bearer = tok.isOk() ? d::Auth::bearer(tok.take())
                             : d::Result<d::Auth>::fail(tok.error());
    if (!bearer.isOk()) {
      emit(d::ResponseEvent(
          d::EvFailed{{toErrorKind(bearer.error().code), "oauth2 token: " + bearer.error().message, {}}}));
      return;
    }
    request_ = d::withAuth(request_, bearer.take());
  }

  domain::IRequestSender *picked = nullptr;
  for (auto *s : deps_.senders)
    if (s && s->supports(request_.type())) { picked = s; break; }
  sender_.store(picked, std::memory_order_release);
  if (!picked) {
    emit(d::ResponseEvent(d::EvFailed{{d::ErrorKind::Unsupported, "no sender for request type", {}}}));
    return;
  }

  state_ = SagaState::Active;
  long long atMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                       (deps_.clock ? deps_.clock->now() : std::chrono::steady_clock::now())
                           .time_since_epoch())
                       .count();
  emit(d::ResponseEvent(d::EvStarted{std::chrono::milliseconds(atMs)}));
  if (token_.cancelled()) {
    emit(d::ResponseEvent(d::EvFailed{{d::ErrorKind::Cancelled, "cancelled", {}}}));
    return;
  }

  // For duplex/stream sessions execute() blocks until close, keeping the saga alive so push/cancel
  // from other threads reach the bound sender.
  FnSink sink;
  sink.fn = emit;
  d::Status s = picked->execute(request_, sink, token_);
  sender_.store(nullptr, std::memory_order_release); // session ended -> no more push/close routing
  // Sender failed without emitting a terminal event -> synthesize one so the observer always sees a terminal.
  if (!s && !terminal())
    emit(d::ResponseEvent(d::EvFailed{{toErrorKind(s.error().code), s.error().message, {}}}));
}

void SendRequestSaga::cancel() {
  token_.cancel();
  // Unblock a live session so the blocked execute() returns (idempotent close).
  if (auto *s = sender_.load(std::memory_order_acquire)) s->close(1000, "cancelled");
}

domain::Status SendRequestSaga::push(domain::WsMessage m) {
  auto *s = sender_.load(std::memory_order_acquire);
  return s ? s->push(std::move(m)) : d::Status::fail({d::ErrorCode::Unsupported, "no sender"});
}
domain::Status SendRequestSaga::halfClose() {
  auto *s = sender_.load(std::memory_order_acquire);
  return s ? s->halfClose() : d::Status::fail({d::ErrorCode::Unsupported, "no sender"});
}
domain::Status SendRequestSaga::close(int code, std::string reason) {
  auto *s = sender_.load(std::memory_order_acquire);
  return s ? s->close(code, std::move(reason))
           : d::Status::fail({d::ErrorCode::Unsupported, "no sender"});
}

} // namespace core::app

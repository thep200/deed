// UiObserver — adapts the new domain port core::domain::IRequestObserver to the existing CoreResponseSink
// (REFACTOR_SPEC P6 UI flip). Reuses ALL existing response handling by converting domain ResponseEvents back
// to the legacy callbacks on the main queue (same marshaling contract as UiDelegateBridge).
//   Unary mode:     EvCompleted -> onCoreResponse,  EvFailed -> onCoreError.
//   Streaming mode: EvStarted -> onStreamOpenTransport (reset pane, '['),  EvMessage -> onStreamChunk,
//                   EvCompleted/EvClosed -> onStreamClose(Ok),  EvFailed -> onStreamClose(error).
// Used for server-stream (gRPC/SSE) and WebSocket sessions. One observer per send (carries its own state).
#pragma once

#import <Cocoa/Cocoa.h>

#include <cstdint>
#include <string>

#include "bridge/CoreBridge.h" // CoreResponseSink + legacy DTOs + StreamStatus
#include "core/domain/ports/driven/i_request_observer.hpp"
#include "core/domain/response/response_event.hpp"

class UiObserver final : public core::domain::IRequestObserver {
public:
  // Unary observer.
  UiObserver(id<CoreResponseSink> sink, std::uint64_t handle)
      : sink_(sink), handle_(handle), streaming_(false) {}
  // Streaming/WS observer. `transport` is display-only (0=grpc,1=sse,2=ws); `token` matches _streamToken.
  UiObserver(id<CoreResponseSink> sink, std::uint64_t token, int transport)
      : sink_(sink), token_(token), transport_(transport), streaming_(true) {}

  void onEvent(core::domain::RequestExecutionId, const core::domain::ResponseEvent &ev) noexcept override {
    using namespace core::domain;
    if (!streaming_) {
      if (const auto *c = ev.get<EvCompleted>()) {
        deliverResponse(c->summary);
      } else if (const auto *f = ev.get<EvFailed>()) {
        deliverError(f->error);
      }
      return;
    }
    // --- streaming ---
    if (ev.is<EvStarted>()) {
      openStream();
    } else if (const auto *m = ev.get<EvMessage>()) {
      if (!opened_) openStream();
      bool first = (count_ == 0);
      ++count_;
      std::string chunk = (first ? std::string("\n  ") : std::string(",\n  ")) + m->payload;
      deliverChunk(chunk, count_);
    } else if (const auto *c = ev.get<EvCompleted>()) {
      if (!opened_) openStream();
      deliverClose(core::StreamStatus::Ok, 0, "", count_, (long long)c->summary.elapsed.count());
    } else if (const auto *cl = ev.get<EvClosed>()) {
      if (!opened_) openStream();
      deliverClose(core::StreamStatus::Ok, cl->code ? *cl->code : 0, cl->reason, count_, 0);
    } else if (const auto *f = ev.get<EvFailed>()) {
      if (!opened_) openStream();
      core::StreamStatus st = (f->error.kind == ErrorKind::Cancelled) ? core::StreamStatus::Cancelled
                              : (f->error.kind == ErrorKind::Timeout) ? core::StreamStatus::Timeout
                                                                      : core::StreamStatus::Error;
      deliverClose(st, f->error.statusCode ? *f->error.statusCode : 0, f->error.message, count_, 0);
    }
  }

private:
  void openStream() {
    if (opened_) return;
    opened_ = true;
    const int t = transport_;
    const std::uint64_t tok = token_;
    __weak id<CoreResponseSink> ws = sink_;
    dispatch_async(dispatch_get_main_queue(), ^{
      id<CoreResponseSink> s = ws;
      if (s) [s onStreamOpenTransport:t token:tok];
    });
  }
  void deliverChunk(const std::string &chunk, std::uint64_t count) {
    NSString *c = [NSString stringWithUTF8String:chunk.c_str()];
    const std::uint64_t tok = token_;
    __weak id<CoreResponseSink> ws = sink_;
    dispatch_async(dispatch_get_main_queue(), ^{
      id<CoreResponseSink> s = ws;
      if (s) [s onStreamChunk:c events:count token:tok];
    });
  }
  void deliverClose(core::StreamStatus status, int code, const std::string &msg, std::uint64_t events,
                    long long elapsedMs) {
    NSString *m = [NSString stringWithUTF8String:msg.c_str()];
    const std::uint64_t tok = token_;
    __weak id<CoreResponseSink> ws = sink_;
    dispatch_async(dispatch_get_main_queue(), ^{
      id<CoreResponseSink> s = ws;
      if (s)
        [s onStreamClose:status code:code message:m events:events elapsedMs:elapsedMs truncated:NO token:tok];
    });
  }
  void deliverResponse(const core::domain::ApiResponse &r) {
    core::domain::ApiResponse copy = r; // own it across the main-queue hop
    const std::uint64_t h = handle_;
    __weak id<CoreResponseSink> ws = sink_;
    dispatch_async(dispatch_get_main_queue(), ^{
      id<CoreResponseSink> s = ws;
      if (s) [s onCoreResponse:h response:copy];
    });
  }
  void deliverError(const core::domain::ApiError &e) {
    core::domain::ApiError copy = e;
    const std::uint64_t h = handle_;
    __weak id<CoreResponseSink> ws = sink_;
    dispatch_async(dispatch_get_main_queue(), ^{
      id<CoreResponseSink> s = ws;
      if (s) [s onCoreError:h error:copy];
    });
  }

  __weak id<CoreResponseSink> sink_;
  std::uint64_t handle_ = 0;
  std::uint64_t token_ = 0;
  int transport_ = 0;
  bool streaming_ = false;
  bool opened_ = false;
  std::uint64_t count_ = 0;
};

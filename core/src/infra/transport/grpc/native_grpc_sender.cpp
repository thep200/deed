#include "infra/transport/grpc/native_grpc_sender.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/dynamic_message.h>

#include <grpcpp/generic/generic_stub.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/support/byte_buffer.h>
#include <grpcpp/support/slice.h>

#include "infra/transport/shared/cancel_token.hpp"
#include "infra/transport/grpc/grpc_descriptors.hpp"
#include "infra/transport/grpc/grpc_send_internal.hpp"

namespace gp = google::protobuf;

namespace core::infra {
namespace d = core::domain;
using namespace grpc_detail;

namespace {

using core::CancelToken;
using grpcdesc::DescriptorContext;
using grpcdesc::makeCreds;

void runUnaryShaped(const d::GrpcRequest &g, const gp::MethodDescriptor *mth, const gp::DescriptorPool *pool,
                    bool clientStream, int deadlineMs, d::IResponseSink &sink,
                    const std::shared_ptr<CancelToken> &cancel) {
  gp::DynamicMessageFactory factory(pool);

  auto channel = grpc::CreateCustomChannel(g.target(), makeCreds(g.tls()), streamChannelArgs());
  grpc::GenericStub stub(channel);
  grpc::ClientContext clientCtx;
  applyCallContext(clientCtx, g, deadlineMs);
  const std::string methodPath = "/" + g.service() + "/" + g.method();

  const auto start = std::chrono::steady_clock::now();
  grpc::ByteBuffer respBuffer;
  grpc::Status status;
  bool gotResp = true;
  std::string err;

  if (clientStream) {
    std::vector<std::string> msgJsons, wire;
    if (!splitMessages(g.message().text(), msgJsons, err)) { emitFailed(sink, d::ErrorKind::Parse, err); return; }
    if (!serializeMessages(msgJsons, mth->input_type(), factory, wire, err)) {
      emitFailed(sink, d::ErrorKind::Parse, err);
      return;
    }
    grpc::CompletionQueue cq;
    std::unique_ptr<GenericRW> rw = stub.PrepareCall(&clientCtx, methodPath, &cq);
    GrpcCall call{*rw, cq, clientCtx, cancel};
    runClientStreamCall(call, wire, respBuffer, gotResp, status);
  } else {
    std::string reqBytes;
    if (!buildRequestBytes(factory, mth, g.message().text(), reqBytes, err)) { emitFailed(sink, d::ErrorKind::Parse, err); return; }
    grpc::Slice reqSlice(reqBytes);
    grpc::ByteBuffer reqBuffer(&reqSlice, 1);
    grpc::CompletionQueue cq;
    auto reader = stub.PrepareUnaryCall(&clientCtx, methodPath, reqBuffer, &cq);
    reader->StartCall();
    void *finishTag = reinterpret_cast<void *>(1);
    reader->Finish(&respBuffer, &status, finishTag);
    awaitUnaryFinish(cq, clientCtx, cancel);
  }

  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - start).count();
  if (cancel && cancel->cancelled()) { emitFailed(sink, d::ErrorKind::Cancelled, "Cancelled"); return; }
  if (!status.ok()) {
    emitFailed(sink, unaryErrKind(status.error_code()),
               "gRPC " + std::to_string(status.error_code()) + ": " + status.error_message());
    return;
  }
  std::string jsonOut;
  if (gotResp && !decodeUnaryResponse(factory, mth, respBuffer, jsonOut, err)) {
    emitFailed(sink, d::ErrorKind::Parse, err);
    return;
  }
  emitUnaryOk(sink, jsonOut, static_cast<long>(elapsed));
}

void runStreaming(const d::GrpcRequest &g, const gp::MethodDescriptor *mth, const gp::DescriptorPool *pool,
                  bool isBidi, int deadlineMs, const GrpcStreamLimits &limits, d::IResponseSink &sink,
                  const std::shared_ptr<CancelToken> &cancel) {
  const auto t0 = std::chrono::steady_clock::now();
  auto offsetMs = [&] {
    return static_cast<long long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
  };

  gp::DynamicMessageFactory factory(pool);
  std::vector<std::string> wire;
  std::string err;
  if (!serializeStreamRequests(g, mth, isBidi, factory, wire, err)) { emitFailed(sink, d::ErrorKind::Parse, err); return; }

  auto channel = grpc::CreateCustomChannel(g.target(), makeCreds(g.tls()), streamChannelArgs());
  grpc::GenericStub stub(channel);
  grpc::ClientContext ctx;
  applyCallContext(ctx, g, deadlineMs);
  const std::string methodPath = "/" + g.service() + "/" + g.method();
  grpc::CompletionQueue cq;
  std::unique_ptr<GenericRW> rw = stub.PrepareCall(&ctx, methodPath, &cq);

  std::unique_ptr<gp::Message> outMsg(factory.GetPrototype(mth->output_type())->New());
  DomainStreamEmitter emitter{sink, ctx, *outMsg, limits.maxEvents, limits.maxBytes};
  auto emit = [&](const grpc::ByteBuffer &m) { return emitter.emit(m); };

  GrpcCall call{*rw, cq, ctx, cancel};
  driveStream(call, isBidi, wire, emit);

  grpc::Status status;
  finishStreamCall(call, status);
  closeStreamDomain(sink, ctx, status, emitter, cancel && cancel->cancelled(), offsetMs());
}

} // namespace

d::Status NativeGrpcSender::executeTyped(const d::RequestModel &resolved, const d::GrpcRequest &g,
                                         d::IResponseSink &sink,
                                         const d::ICancellationToken &cancel) {
  const int deadlineMs = static_cast<int>(resolved.config().timeout.millis());

  auto token = core::linkCancel(cancel);

  // Route by the DESCRIPTOR's streaming flags — authoritative over the request's declared methodType.
  DescriptorContext ctx;
  ctx.cancel = token; // reflection is a network call too -> Cancel must abort it, not wait out its 30s deadline
  std::string err;
  const gp::MethodDescriptor *mth = resolveMethod(g, ctx, err);
  if (token->cancelled()) {
    emitFailed(sink, d::ErrorKind::Cancelled, "Cancelled");
    return d::ok();
  }
  if (mth) {
    const bool srv = mth->server_streaming();
    const bool cli = mth->client_streaming();
    if (srv) runStreaming(g, mth, ctx.activePool, /*isBidi*/ cli, deadlineMs, limits_, sink, token); // server-stream or bidi
    else runUnaryShaped(g, mth, ctx.activePool, /*clientStream*/ cli, deadlineMs, sink, token); // unary or client-stream
  } else {
    emitFailed(sink, d::ErrorKind::Parse, err);
  }
  return d::ok();
}

} // namespace core::infra

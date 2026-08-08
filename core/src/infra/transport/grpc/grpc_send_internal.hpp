#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/dynamic_message.h>

#include <grpcpp/generic/generic_stub.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/support/byte_buffer.h>
#include <grpcpp/support/channel_arguments.h>

#include "infra/transport/grpc/grpc_descriptors.hpp"
#include "infra/transport/shared/cancel_token.hpp"
#include "infra/transport/typed_sender.hpp"

namespace gp = google::protobuf;

namespace core::infra::grpc_detail {
namespace d = core::domain;

using core::CancelToken;
using grpcdesc::DescriptorContext;

d::ErrorKind unaryErrKind(grpc::StatusCode code);
d::ErrorKind streamErrKind(const grpc::Status &st);
void emitFailed(d::IResponseSink &sink, d::ErrorKind k, const std::string &msg,
                std::optional<int> status = std::nullopt);
void emitUnaryOk(d::IResponseSink &sink, const std::string &jsonOut, long elapsedMs);

grpc::ChannelArguments streamChannelArgs();
std::string byteBufferToString(const grpc::ByteBuffer &bb);
int pumpCq(grpc::CompletionQueue &cq, const std::shared_ptr<CancelToken> &cancel, grpc::ClientContext &ctx);
void shutdownAndDrain(grpc::CompletionQueue &cq);

bool splitMessages(const std::string &message, std::vector<std::string> &out, std::string &err);
bool serializeMessages(const std::vector<std::string> &jsons, const gp::Descriptor *type,
                       gp::DynamicMessageFactory &factory, std::vector<std::string> &wire, std::string &err);
const gp::MethodDescriptor *resolveMethod(const d::GrpcRequest &g, DescriptorContext &ctx, std::string &err);
void applyCallContext(grpc::ClientContext &ctx, const d::GrpcRequest &g, int deadlineMs);
bool decodeUnaryResponse(gp::DynamicMessageFactory &factory, const gp::MethodDescriptor *mth,
                         const grpc::ByteBuffer &respBuffer, std::string &jsonOut, std::string &err);
bool buildRequestBytes(gp::DynamicMessageFactory &factory, const gp::MethodDescriptor *mth,
                       const std::string &messageJson, std::string &outBytes, std::string &err);
void awaitUnaryFinish(grpc::CompletionQueue &cq, grpc::ClientContext &ctx,
                      const std::shared_ptr<CancelToken> &cancel);

using GenericRW = grpc::ClientAsyncReaderWriter<grpc::ByteBuffer, grpc::ByteBuffer>;
using EmitFn = std::function<bool(const grpc::ByteBuffer &)>;

struct GrpcCall {
  GenericRW &rw;
  grpc::CompletionQueue &cq;
  grpc::ClientContext &ctx;
  const std::shared_ptr<CancelToken> &cancel;
};

void runClientStreamCall(GrpcCall &call, const std::vector<std::string> &wire, grpc::ByteBuffer &respBuf,
                         bool &gotResp, grpc::Status &status);

// Accumulates streamed events as domain EvMessage, enforcing the event/byte ceilings.
struct DomainStreamEmitter {
  d::IResponseSink &sink;
  grpc::ClientContext &ctx;
  gp::Message &outMsg;
  std::uint64_t maxEvents;
  std::uint64_t maxBytes;
  std::uint64_t seq = 0;
  std::uint64_t bytes = 0;
  bool truncated = false;
  bool emit(const grpc::ByteBuffer &msg);
};

bool serializeStreamRequests(const d::GrpcRequest &g, const gp::MethodDescriptor *mth, bool isBidi,
                             gp::DynamicMessageFactory &factory, std::vector<std::string> &wire, std::string &err);
void driveStream(GrpcCall &call, bool isBidi, const std::vector<std::string> &wire, const EmitFn &emit);
void finishStreamCall(GrpcCall &call, grpc::Status &status);
void closeStreamDomain(d::IResponseSink &sink, grpc::ClientContext &ctx, const grpc::Status &status,
                       const DomainStreamEmitter &em, bool cancelled, long long elapsedMs);

} // namespace core::infra::grpc_detail

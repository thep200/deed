#include "infra/transport/grpc/grpc_send_internal.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/util/json_util.h>

#include <grpcpp/grpcpp.h>
#include <grpcpp/resource_quota.h>
#include <grpcpp/support/byte_buffer.h>
#include <grpcpp/support/channel_arguments.h>
#include <grpcpp/support/slice.h>

#include "infra/transport/grpc/grpc_descriptors.hpp"

namespace core::infra::grpc_detail {

// Fallback when the per-request timeout is unset/invalid (normally seeded from .env DEFAULT_TIMEOUT_MS).
constexpr int kFallbackDeadlineMs = 30000;
// Hard channel-memory quota for streaming calls (not a user tunable — protects the app, not the request).
constexpr std::uint64_t kStreamChannelQuotaBytes = 512ull * 1024 * 1024;

d::ErrorKind unaryErrKind(grpc::StatusCode code) {
  switch (code) {
  case grpc::StatusCode::DEADLINE_EXCEEDED: return d::ErrorKind::Timeout;
  case grpc::StatusCode::CANCELLED: return d::ErrorKind::Cancelled;
  case grpc::StatusCode::UNAVAILABLE: return d::ErrorKind::Network;
  default: return d::ErrorKind::Network;
  }
}
d::ErrorKind streamErrKind(const grpc::Status &st) {
  switch (st.error_code()) {
  case grpc::StatusCode::DEADLINE_EXCEEDED: return d::ErrorKind::Timeout;
  case grpc::StatusCode::CANCELLED: return d::ErrorKind::Cancelled;
  default: return d::ErrorKind::Protocol;
  }
}
void emitFailed(d::IResponseSink &sink, d::ErrorKind k, const std::string &msg,
                std::optional<int> status) {
  sink.emit(d::ResponseEvent(d::EvFailed{{k, msg, status}}));
}
void emitUnaryOk(d::IResponseSink &sink, const std::string &jsonOut, long elapsedMs) {
  d::ApiResponse resp;
  resp.statusCode = 0; // gRPC OK
  resp.body = jsonOut;
  resp.elapsed = std::chrono::milliseconds(elapsedMs);
  sink.emit(d::ResponseEvent(d::EvCompleted{std::move(resp)}));
}

grpc::ChannelArguments streamChannelArgs() {
  grpc::ChannelArguments args;
  grpc::ResourceQuota quota("deed-grpc");
  quota.Resize(kStreamChannelQuotaBytes);
  args.SetResourceQuota(quota);
  return args;
}

std::string byteBufferToString(const grpc::ByteBuffer &bb) {
  std::vector<grpc::Slice> slices;
  if (!bb.Dump(&slices).ok()) return {};
  std::string out;
  out.reserve(bb.Length()); // one allocation for the whole message (avoids per-slice reallocs)
  for (const auto &s : slices) out.append(reinterpret_cast<const char *>(s.begin()), s.size());
  return out;
}

// Pump the CQ until the next completion; honor cancel via TryCancel. 1=ok, 0=!ok, -1=shutdown.
int pumpCq(grpc::CompletionQueue &cq, const std::shared_ptr<CancelToken> &cancel, grpc::ClientContext &ctx) {
  for (;;) {
    void *tag = nullptr;
    bool ok = false;
    auto deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(100);
    auto st = cq.AsyncNext(&tag, &ok, deadline);
    if (st == grpc::CompletionQueue::SHUTDOWN) return -1;
    if (st == grpc::CompletionQueue::GOT_EVENT) return ok ? 1 : 0;
    if (cancel && cancel->cancelled()) ctx.TryCancel();
  }
}

void shutdownAndDrain(grpc::CompletionQueue &cq) {
  cq.Shutdown();
  void *t = nullptr;
  bool o = false;
  while (cq.Next(&t, &o)) {}
}

bool splitMessages(const std::string &message, std::vector<std::string> &out, std::string &err) {
  if (message.find_first_not_of(" \t\r\n") == std::string::npos) return true; // empty -> 0 messages
  nlohmann::json parsed;
  try {
    parsed = nlohmann::json::parse(message);
  } catch (const std::exception &e) {
    err = std::string("invalid JSON message: ") + e.what();
    return false;
  }
  if (parsed.is_array())
    for (const auto &el : parsed) out.push_back(el.dump());
  else
    out.push_back(parsed.dump());
  return true;
}

bool serializeMessages(const std::vector<std::string> &jsons, const gp::Descriptor *type,
                       gp::DynamicMessageFactory &factory, std::vector<std::string> &wire, std::string &err) {
  wire.reserve(jsons.size());
  // Resolve the prototype + allocate one scratch message once; Clear()+reparse per element.
  const gp::Message *proto = factory.GetPrototype(type);
  std::unique_ptr<gp::Message> rm(proto->New());
  gp::util::JsonParseOptions popts;
  popts.ignore_unknown_fields = true;
  for (size_t i = 0; i < jsons.size(); ++i) {
    rm->Clear();
    auto st = gp::util::JsonStringToMessage(jsons[i], rm.get(), popts);
    if (!st.ok()) {
      err = "invalid message #" + std::to_string(i) + " for " + std::string(type->full_name()) + ": " +
            std::string(st.message());
      return false;
    }
    std::string bytes;
    if (!rm->SerializeToString(&bytes)) { err = "failed to serialize message #" + std::to_string(i); return false; }
    wire.push_back(std::move(bytes));
  }
  return true;
}

const gp::MethodDescriptor *resolveMethod(const d::GrpcRequest &g, DescriptorContext &ctx, std::string &err) {
  if (!grpcdesc::buildDescriptors(g, ctx)) { err = ctx.error; return nullptr; }
  const gp::ServiceDescriptor *svc = ctx.activePool->FindServiceByName(g.service());
  if (!svc) { err = "service not found: " + g.service(); return nullptr; }
  const gp::MethodDescriptor *mth = svc->FindMethodByName(g.method());
  if (!mth) { err = "method not found: " + g.method(); return nullptr; }
  return mth;
}

void applyCallContext(grpc::ClientContext &ctx, const d::GrpcRequest &g, int deadlineMs) {
  if (deadlineMs <= 0) deadlineMs = kFallbackDeadlineMs;
  ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(deadlineMs));
  for (const auto &md : g.metadata().entries())
    if (md.enabled && !md.key.empty()) ctx.AddMetadata(md.key, md.value);
}

namespace {

bool messageToPrettyJson(const gp::Message &msg, std::string &out, std::string &err) {
  gp::util::JsonPrintOptions jopts;
  jopts.add_whitespace = true;
  jopts.always_print_fields_with_no_presence = true;
  auto st = gp::util::MessageToJsonString(msg, &out, jopts);
  if (!st.ok()) { err = "failed to convert response to JSON: " + std::string(st.message()); return false; }
  return true;
}

} // namespace

bool decodeUnaryResponse(gp::DynamicMessageFactory &factory, const gp::MethodDescriptor *mth,
                         const grpc::ByteBuffer &respBuffer, std::string &jsonOut, std::string &err) {
  std::unique_ptr<gp::Message> respMsg(factory.GetPrototype(mth->output_type())->New());
  if (!respMsg->ParseFromString(byteBufferToString(respBuffer))) { err = "failed to parse protobuf response"; return false; }
  return messageToPrettyJson(*respMsg, jsonOut, err);
}

bool buildRequestBytes(gp::DynamicMessageFactory &factory, const gp::MethodDescriptor *mth,
                       const std::string &messageJson, std::string &outBytes, std::string &err) {
  std::unique_ptr<gp::Message> reqMsg(factory.GetPrototype(mth->input_type())->New());
  gp::util::JsonParseOptions popts;
  popts.ignore_unknown_fields = true;
  auto st = gp::util::JsonStringToMessage(messageJson.empty() ? "{}" : messageJson, reqMsg.get(), popts);
  if (!st.ok()) {
    err = "invalid JSON message for " + std::string(mth->input_type()->full_name()) + ": " +
          std::string(st.message());
    return false;
  }
  if (!reqMsg->SerializeToString(&outBytes)) { err = "failed to serialize protobuf request"; return false; }
  return true;
}

void awaitUnaryFinish(grpc::CompletionQueue &cq, grpc::ClientContext &ctx,
                      const std::shared_ptr<CancelToken> &cancel) {
  pumpCq(cq, cancel, ctx);
  shutdownAndDrain(cq);
}

} // namespace core::infra::grpc_detail

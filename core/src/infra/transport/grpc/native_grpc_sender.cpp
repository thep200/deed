#include "infra/transport/grpc/native_grpc_sender.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/util/json_util.h>

#include <grpcpp/generic/generic_stub.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/resource_quota.h>
#include <grpcpp/support/byte_buffer.h>
#include <grpcpp/support/channel_arguments.h>
#include <grpcpp/support/slice.h>

#include "infra/transport/shared/cancel_token.hpp"
#include "infra/transport/grpc/grpc_descriptors.hpp"

namespace gp = google::protobuf;

namespace core::infra {
namespace d = core::domain;

namespace {

using core::CancelToken;
using grpcdesc::DescriptorContext;
using grpcdesc::makeCreds;

// Fallback call deadline when the per-request timeout is unset/invalid (per-request timeout normally
// arrives via RequestConfig, seeded from .env DEFAULT_TIMEOUT_MS).
constexpr int kFallbackDeadlineMs = 30000;
// Hard channel-memory quota for streaming calls (not a user tunable — protects the app, not the request).
constexpr std::uint64_t kStreamChannelQuotaBytes = 512ull * 1024 * 1024;

// ---- domain ResponseEvent helpers (mirror LegacySenderAdapter's translation) ----

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
                std::optional<int> status = std::nullopt) {
  sink.emit(d::ResponseEvent(d::EvFailed{{k, msg, status}}));
}
void emitUnaryOk(d::IResponseSink &sink, const std::string &jsonOut, long elapsedMs) {
  d::ApiResponse resp;
  resp.statusCode = 0; // gRPC OK
  resp.body = jsonOut;
  resp.elapsed = std::chrono::milliseconds(elapsedMs);
  sink.emit(d::ResponseEvent(d::EvCompleted{std::move(resp)}));
}

// ---- grpc++ machinery (ported verbatim from the legacy GrpcSender; only the emit points are domain now) ----

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
  // Resolve the prototype + allocate one scratch message ONCE, then Clear()+reparse per element
  // (hoisted out of the loop; the old code re-fetched the prototype and heap-allocated a message per msg).
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

bool messageToPrettyJson(const gp::Message &msg, std::string &out, std::string &err) {
  gp::util::JsonPrintOptions jopts;
  jopts.add_whitespace = true;
  jopts.always_print_fields_with_no_presence = true;
  auto st = gp::util::MessageToJsonString(msg, &out, jopts);
  if (!st.ok()) { err = "failed to convert response to JSON: " + std::string(st.message()); return false; }
  return true;
}

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

using GenericRW = grpc::ClientAsyncReaderWriter<grpc::ByteBuffer, grpc::ByteBuffer>;
using EmitFn = std::function<bool(const grpc::ByteBuffer &)>;

struct GrpcCall {
  GenericRW &rw;
  grpc::CompletionQueue &cq;
  grpc::ClientContext &ctx;
  const std::shared_ptr<CancelToken> &cancel;
};

void runClientStreamCall(GrpcCall &call, const std::vector<std::string> &wire, grpc::ByteBuffer &respBuf,
                         bool &gotResp, grpc::Status &status) {
  void *tag = reinterpret_cast<void *>(1);
  call.rw.StartCall(tag);
  bool failed = (pumpCq(call.cq, call.cancel, call.ctx) != 1);
  for (size_t i = 0; !failed && i < wire.size(); ++i) {
    if (call.cancel && call.cancel->cancelled()) break;
    grpc::Slice sl(wire[i]);
    grpc::ByteBuffer buf(&sl, 1);
    call.rw.Write(buf, tag);
    if (pumpCq(call.cq, call.cancel, call.ctx) != 1) { failed = true; break; }
  }
  if (!failed) { call.rw.WritesDone(tag); pumpCq(call.cq, call.cancel, call.ctx); }
  gotResp = false;
  if (!failed) { call.rw.Read(&respBuf, tag); gotResp = (pumpCq(call.cq, call.cancel, call.ctx) == 1); }
  call.rw.Finish(&status, tag);
  pumpCq(call.cq, call.cancel, call.ctx);
  shutdownAndDrain(call.cq);
}

void runServerStreamReads(GrpcCall &call, const std::string &reqWire, const EmitFn &emit) {
  void *kTag = reinterpret_cast<void *>(1);
  grpc::Slice sl(reqWire);
  grpc::ByteBuffer reqBuffer(&sl, 1);
  call.rw.Write(reqBuffer, kTag);
  if (pumpCq(call.cq, call.cancel, call.ctx) != 1) return;
  call.rw.WritesDone(kTag);
  if (pumpCq(call.cq, call.cancel, call.ctx) != 1) return;
  while (true) {
    if (call.cancel && call.cancel->cancelled()) { call.ctx.TryCancel(); break; }
    grpc::ByteBuffer msg;
    call.rw.Read(&msg, kTag);
    if (pumpCq(call.cq, call.cancel, call.ctx) != 1) break;
    if (!emit(msg)) break;
  }
}

void *bidiTag(int n) { return reinterpret_cast<void *>(static_cast<std::intptr_t>(n)); }

struct BidiPump {
  GenericRW &rw;
  const std::vector<std::string> &wire;
  grpc::ByteBuffer writeBuf;
  grpc::ByteBuffer readBuf;
  size_t widx = 0;
  enum WState { WRITING, WDONE_PENDING, WDONE_DONE };
  WState wstate = WRITING;
  bool reading = true;
};

void bidiStartWrite(BidiPump &p, size_t idx) {
  grpc::Slice sl(p.wire[idx]);
  p.writeBuf = grpc::ByteBuffer(&sl, 1);
  p.rw.Write(p.writeBuf, bidiTag(1));
}
void bidiOnWrite(BidiPump &p, bool ok) {
  if (!ok) { p.wstate = BidiPump::WDONE_DONE; return; }
  if (++p.widx < p.wire.size()) { bidiStartWrite(p, p.widx); return; }
  p.rw.WritesDone(bidiTag(3));
  p.wstate = BidiPump::WDONE_PENDING;
}
void bidiOnRead(BidiPump &p, bool ok, GrpcCall &call, const EmitFn &emit) {
  if (!ok) { p.reading = false; return; }
  if (call.cancel && call.cancel->cancelled()) { call.ctx.TryCancel(); p.reading = false; return; }
  if (!emit(p.readBuf)) { p.reading = false; return; }
  p.rw.Read(&p.readBuf, bidiTag(2));
}
void runBidiStream(GrpcCall &call, const std::vector<std::string> &wire, const EmitFn &emit) {
  BidiPump p{call.rw, wire};
  if (!wire.empty()) bidiStartWrite(p, 0);
  else { call.rw.WritesDone(bidiTag(3)); p.wstate = BidiPump::WDONE_PENDING; }
  call.rw.Read(&p.readBuf, bidiTag(2));
  while (p.wstate != BidiPump::WDONE_DONE || p.reading) {
    void *tag = nullptr;
    bool ok = false;
    auto deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(100);
    auto st = call.cq.AsyncNext(&tag, &ok, deadline);
    if (st == grpc::CompletionQueue::SHUTDOWN) break;
    if (st == grpc::CompletionQueue::TIMEOUT) {
      if (call.cancel && call.cancel->cancelled()) call.ctx.TryCancel();
      continue;
    }
    if (tag == bidiTag(1)) bidiOnWrite(p, ok);
    else if (tag == bidiTag(3)) p.wstate = BidiPump::WDONE_DONE;
    else if (tag == bidiTag(2)) bidiOnRead(p, ok, call, emit);
    if (call.cancel && call.cancel->cancelled()) call.ctx.TryCancel();
  }
}

std::string decodeStreamMessage(gp::Message &outMsg, const grpc::ByteBuffer &msg, std::uint64_t seq) {
  outMsg.Clear();
  bool decoded = outMsg.ParseFromString(byteBufferToString(msg));
  std::string json;
  if (decoded) {
    gp::util::JsonPrintOptions jopts;
    jopts.add_whitespace = false;
    jopts.always_print_fields_with_no_presence = true;
    decoded = gp::util::MessageToJsonString(outMsg, &json, jopts).ok();
  }
  if (!decoded)
    json = "{\"__decode_error__\":\"failed to decode protobuf message\",\"seq\":" + std::to_string(seq) + "}";
  return json;
}

// Accumulates streamed events as domain EvMessage, enforcing the §9 event/byte ceilings.
struct DomainStreamEmitter {
  d::IResponseSink &sink;
  grpc::ClientContext &ctx;
  gp::Message &outMsg;
  std::uint64_t maxEvents;
  std::uint64_t maxBytes;
  std::uint64_t seq = 0;
  std::uint64_t bytes = 0;
  bool truncated = false;
  bool emit(const grpc::ByteBuffer &msg) {
    std::string json = decodeStreamMessage(outMsg, msg, seq);
    sink.emit(d::ResponseEvent(d::EvMessage{d::WsSendKind::Text, json, static_cast<size_t>(seq)}));
    ++seq;
    bytes += json.size();
    if (seq >= maxEvents || bytes >= maxBytes) { truncated = true; ctx.TryCancel(); return false; }
    return true;
  }
};

bool serializeStreamRequests(const d::GrpcRequest &g, const gp::MethodDescriptor *mth, bool isBidi,
                             gp::DynamicMessageFactory &factory, std::vector<std::string> &wire, std::string &err) {
  if (isBidi) {
    std::vector<std::string> jsons;
    return splitMessages(g.message().text(), jsons, err) &&
           serializeMessages(jsons, mth->input_type(), factory, wire, err);
  }
  std::string b;
  if (!buildRequestBytes(factory, mth, g.message().text(), b, err)) return false;
  wire.push_back(std::move(b));
  return true;
}

void driveStream(GrpcCall &call, bool isBidi, const std::vector<std::string> &wire, const EmitFn &emit) {
  void *kStartTag = reinterpret_cast<void *>(10);
  call.rw.StartCall(kStartTag);
  if (pumpCq(call.cq, call.cancel, call.ctx) != 1) return;
  if (isBidi) runBidiStream(call, wire, emit);
  else runServerStreamReads(call, wire[0], emit);
}

void finishStreamCall(GrpcCall &call, grpc::Status &status) {
  void *kFinishTag = reinterpret_cast<void *>(11);
  call.rw.Finish(&status, kFinishTag);
  pumpCq(call.cq, call.cancel, call.ctx);
  shutdownAndDrain(call.cq);
}

void closeStreamDomain(d::IResponseSink &sink, grpc::ClientContext &ctx, const grpc::Status &status,
                       const DomainStreamEmitter &em, bool cancelled, long long elapsedMs) {
  // Trailing metadata -> EvTrailers (skip binary "-bin" values, as the legacy did).
  std::vector<d::ResponseHeader> ts;
  for (const auto &kv : ctx.GetServerTrailingMetadata()) {
    std::string key(kv.first.data(), kv.first.size());
    if (key.size() >= 4 && key.compare(key.size() - 4, 4, "-bin") == 0) continue;
    ts.push_back({std::move(key), std::string(kv.second.data(), kv.second.size())});
  }
  if (!ts.empty()) sink.emit(d::ResponseEvent(d::EvTrailers{std::move(ts)}));

  // Truncation is a successful cap-hit, not a failure (§10).
  if (cancelled && !em.truncated) { emitFailed(sink, d::ErrorKind::Cancelled, status.error_message()); return; }
  if (status.ok() || em.truncated) {
    d::ApiResponse summary;
    summary.statusCode = status.error_code();
    summary.elapsed = std::chrono::milliseconds(elapsedMs);
    sink.emit(d::ResponseEvent(d::EvCompleted{std::move(summary)}));
  } else {
    emitFailed(sink, streamErrKind(status), status.error_message(), status.error_code());
  }
}

// ---- unary / client-stream (unary-shaped result) ----
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

// ---- server-stream / bidi (EvMessage* then terminal) ----
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

d::Status NativeGrpcSender::execute(const d::RequestModel &resolved, d::IResponseSink &sink,
                                    const d::ICancellationToken &cancel) {
  if (resolved.type() != d::RequestType::Grpc) {
    emitFailed(sink, d::ErrorKind::Unsupported, "NativeGrpcSender: not a gRPC request");
    return d::ok();
  }
  const d::GrpcRequest &g = std::get<d::GrpcRequest>(resolved.payload());
  const int deadlineMs = static_cast<int>(resolved.config().timeout.millis());

  auto token = std::make_shared<CancelToken>();
  if (cancel.cancelled()) token->cancel();
  { std::lock_guard<std::mutex> lk(mu_); token_ = token; }

  // Resolve the method, then route by the DESCRIPTOR's streaming flags (authoritative — fixes bidi, which
  // the legacy string-based isStreaming() mis-routed to the unary send path).
  DescriptorContext ctx;
  std::string err;
  const gp::MethodDescriptor *mth = resolveMethod(g, ctx, err);
  if (mth) {
    const bool srv = mth->server_streaming();
    const bool cli = mth->client_streaming();
    if (srv) runStreaming(g, mth, ctx.activePool, /*isBidi*/ cli, deadlineMs, limits_, sink, token); // server-stream or bidi
    else runUnaryShaped(g, mth, ctx.activePool, /*clientStream*/ cli, deadlineMs, sink, token); // unary or client-stream
  } else {
    emitFailed(sink, d::ErrorKind::Parse, err);
  }

  { std::lock_guard<std::mutex> lk(mu_); token_.reset(); }
  return d::ok();
}

d::Status NativeGrpcSender::close(int, std::string) {
  std::lock_guard<std::mutex> lk(mu_);
  if (token_) token_->cancel();
  return d::ok();
}

} // namespace core::infra

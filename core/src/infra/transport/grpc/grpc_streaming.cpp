#include "infra/transport/grpc/grpc_send_internal.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/util/json_util.h>

#include <grpcpp/generic/generic_stub.h>
#include <grpcpp/grpcpp.h>
#include <grpcpp/support/byte_buffer.h>
#include <grpcpp/support/slice.h>

namespace core::infra::grpc_detail {

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

namespace {

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

} // namespace

bool DomainStreamEmitter::emit(const grpc::ByteBuffer &msg) {
  std::string json = decodeStreamMessage(outMsg, msg, seq);
  sink.emit(d::ResponseEvent(d::EvMessage{d::WsSendKind::Text, json, static_cast<size_t>(seq)}));
  ++seq;
  bytes += json.size();
  if (seq >= maxEvents || bytes >= maxBytes) { truncated = true; ctx.TryCancel(); return false; }
  return true;
}

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
  // Trailing metadata -> EvTrailers (binary "-bin" values skipped).
  std::vector<d::ResponseHeader> ts;
  for (const auto &kv : ctx.GetServerTrailingMetadata()) {
    std::string key(kv.first.data(), kv.first.size());
    if (key.size() >= 4 && key.compare(key.size() - 4, 4, "-bin") == 0) continue;
    ts.push_back({std::move(key), std::string(kv.second.data(), kv.second.size())});
  }
  if (!ts.empty()) sink.emit(d::ResponseEvent(d::EvTrailers{std::move(ts)}));

  // Truncation is a successful cap-hit, not a failure.
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

} // namespace core::infra::grpc_detail

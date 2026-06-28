#include "sending/grpc_sender.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
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

#include "codec/json_codec.hpp"
#include "sending/grpc_descriptors.hpp"

namespace gp = google::protobuf;

namespace core {

namespace {

using grpcdesc::DescriptorContext;
using grpcdesc::makeCreds;

ErrorKind mapGrpcStatus(grpc::StatusCode code) {
    switch (code) {
        case grpc::StatusCode::DEADLINE_EXCEEDED: return ErrorKind::Timeout;
        case grpc::StatusCode::CANCELLED: return ErrorKind::Cancelled;
        case grpc::StatusCode::UNAVAILABLE: return ErrorKind::Network;
        default: return ErrorKind::Network;
    }
}

// Channel args shared by every gRPC call. A ResourceQuota caps the memory one channel may use for
// buffering, so a buggy/flooding server can't OOM the app (perf spec §2.8/§7). Flow control already
// bounds in-flight data; this is the hard backstop. 512 MiB is generous for a desktop client.
grpc::ChannelArguments streamChannelArgs() {
    grpc::ChannelArguments args;
    grpc::ResourceQuota quota("deed-grpc");
    quota.Resize(512ull * 1024 * 1024);
    args.SetResourceQuota(quota);
    return args;
}

std::string byteBufferToString(const grpc::ByteBuffer& bb) {
    std::vector<grpc::Slice> slices;
    if (!bb.Dump(&slices).ok()) return {};
    std::string out;
    for (const auto& s : slices) out.append(reinterpret_cast<const char*>(s.begin()), s.size());
    return out;
}

// ---- Streaming helpers (SPEC_grpc_streaming §5/§6) ----

long long nowEpochMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// gRPC trailing metadata -> neutral KeyValue list (binary "-bin" values are skipped, not base64'd in v1).
std::vector<KeyValue> trailingToKv(const std::multimap<grpc::string_ref, grpc::string_ref>& md) {
    std::vector<KeyValue> out;
    for (const auto& kv : md) {
        std::string key(kv.first.data(), kv.first.size());
        if (key.size() >= 4 && key.compare(key.size() - 4, 4, "-bin") == 0) continue;
        out.push_back(KeyValue{std::move(key), std::string(kv.second.data(), kv.second.size()), true});
    }
    return out;
}

StreamStatus mapStreamStatus(const grpc::Status& st, bool cancelled) {
    if (cancelled) return StreamStatus::Cancelled;
    switch (st.error_code()) {
        case grpc::StatusCode::OK: return StreamStatus::Ok;
        case grpc::StatusCode::CANCELLED: return StreamStatus::Cancelled;
        case grpc::StatusCode::DEADLINE_EXCEEDED: return StreamStatus::Timeout;
        default: return StreamStatus::Error;
    }
}

// One generic async reader/writer over ByteBuffer (server-streaming uses Write once then Read*).
using GenericRW = grpc::ClientAsyncReaderWriter<grpc::ByteBuffer, grpc::ByteBuffer>;

// Pump the CQ until the next completion. While waiting, honor cancel by TryCancel (mirrors unary §6).
// Returns: 1 = event ok, 0 = event !ok (op failed / stream end), -1 = CQ shutdown.
int pumpCq(grpc::CompletionQueue& cq, const std::shared_ptr<CancelToken>& cancel,
           grpc::ClientContext& ctx) {
    for (;;) {
        void* tag = nullptr;
        bool ok = false;
        auto deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(100);
        auto st = cq.AsyncNext(&tag, &ok, deadline);
        if (st == grpc::CompletionQueue::SHUTDOWN) return -1;
        if (st == grpc::CompletionQueue::GOT_EVENT) return ok ? 1 : 0;
        if (cancel && cancel->cancelled()) ctx.TryCancel();   // TIMEOUT -> check cancel, loop
    }
}

// Split the message field into individual request-message JSON strings (array | single | empty).
// Returns false + err on malformed JSON. Empty/whitespace -> zero messages (out stays empty).
bool splitMessages(const std::string& message, std::vector<std::string>& out, std::string& err) {
    if (message.find_first_not_of(" \t\r\n") == std::string::npos) return true;  // empty -> 0 messages
    nlohmann::json parsed;
    try {
        parsed = nlohmann::json::parse(message);
    } catch (const std::exception& e) {
        err = std::string("invalid JSON message: ") + e.what();
        return false;
    }
    if (parsed.is_array())
        for (const auto& el : parsed) out.push_back(el.dump());
    else
        out.push_back(parsed.dump());   // single message
    return true;
}

// Serialize each element JSON to protobuf wire bytes for `type`. Returns false + err (with index) on failure.
bool serializeMessages(const std::vector<std::string>& jsons, const gp::Descriptor* type,
                       gp::DynamicMessageFactory& factory, std::vector<std::string>& wire,
                       std::string& err) {
    wire.reserve(jsons.size());
    for (size_t i = 0; i < jsons.size(); ++i) {
        std::unique_ptr<gp::Message> rm(factory.GetPrototype(type)->New());
        gp::util::JsonParseOptions popts;
        popts.ignore_unknown_fields = true;
        auto st = gp::util::JsonStringToMessage(jsons[i], rm.get(), popts);
        if (!st.ok()) {
            err = "invalid message #" + std::to_string(i) + " for " + std::string(type->full_name()) +
                  ": " + std::string(st.message());
            return false;
        }
        std::string bytes;
        if (!rm->SerializeToString(&bytes)) {
            err = "failed to serialize message #" + std::to_string(i);
            return false;
        }
        wire.push_back(std::move(bytes));
    }
    return true;
}

// Load descriptors + find the requested method. Returns nullptr and sets `err` on any failure.
const gp::MethodDescriptor* resolveMethod(const GrpcRequest& g, DescriptorContext& ctx, std::string& err) {
    if (!grpcdesc::buildDescriptors(g, ctx)) { err = ctx.error; return nullptr; }
    const gp::ServiceDescriptor* svc = ctx.activePool->FindServiceByName(g.service);
    if (!svc) { err = "service not found: " + g.service; return nullptr; }
    const gp::MethodDescriptor* mth = svc->FindMethodByName(g.method);
    if (!mth) { err = "method not found: " + g.method; return nullptr; }
    return mth;
}

// Apply the per-call deadline (Config timeout, default 30s) + request metadata to a fresh ClientContext.
void applyCallContext(grpc::ClientContext& ctx, const GrpcRequest& g) {
    int deadlineMs = g.settings.deadlineMs > 0 ? g.settings.deadlineMs : 30000;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(deadlineMs));
    for (const auto& md : g.metadata)
        if (md.enabled && !md.key.empty()) ctx.AddMetadata(md.key, md.value);
}

// protobuf Message -> pretty JSON (unary/client-stream responses). false + err on failure.
bool messageToPrettyJson(const gp::Message& msg, std::string& out, std::string& err) {
    gp::util::JsonPrintOptions jopts;
    jopts.add_whitespace = true;
    jopts.always_print_fields_with_no_presence = true;
    auto st = gp::util::MessageToJsonString(msg, &out, jopts);
    if (!st.ok()) { err = "failed to convert response to JSON: " + std::string(st.message()); return false; }
    return true;
}

// Parse unary response bytes into a fresh output message and convert to pretty JSON. false + err on failure.
bool decodeUnaryResponse(gp::DynamicMessageFactory& factory, const gp::MethodDescriptor* mth,
                         const grpc::ByteBuffer& respBuffer, std::string& jsonOut, std::string& err) {
    std::unique_ptr<gp::Message> respMsg(factory.GetPrototype(mth->output_type())->New());
    if (!respMsg->ParseFromString(byteBufferToString(respBuffer))) {
        err = "failed to parse protobuf response";
        return false;
    }
    return messageToPrettyJson(*respMsg, jsonOut, err);
}

// Deliver a successful unary/client-stream result (gRPC OK, JSON body) to the delegate.
void deliverUnaryOk(IUiDelegate& delegate, RequestHandle handle, const RequestModel& model,
                    const std::string& jsonOut, long elapsedMs) {
    ApiResponse resp;
    resp.statusCode = 0;   // gRPC OK
    resp.statusText = "OK";
    resp.body = jsonOut;
    resp.elapsedMs = elapsedMs;
    resp.sizeBytes = static_cast<std::int64_t>(jsonOut.size());
    resp.resolvedRequestDump = codec::dumpRequest(model);
    delegate.onResponse(handle, resp);
}

// JSON request -> serialized protobuf wire bytes for the method's input type. false + err on failure.
bool buildRequestBytes(gp::DynamicMessageFactory& factory, const gp::MethodDescriptor* mth,
                       const std::string& messageJson, std::string& outBytes, std::string& err) {
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

// Drive the CQ for a unary call until the single Finish event arrives (or CQ shutdown); cancel -> TryCancel.
// Shuts the CQ down and drains it before returning.
void awaitUnaryFinish(grpc::CompletionQueue& cq, grpc::ClientContext& ctx,
                      const std::shared_ptr<CancelToken>& cancel) {
    bool done = false;
    while (!done) {
        void* tag = nullptr;
        bool ok = false;
        auto deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(100);
        auto next = cq.AsyncNext(&tag, &ok, deadline);
        if (next == grpc::CompletionQueue::GOT_EVENT) done = true;
        else if (next == grpc::CompletionQueue::SHUTDOWN) break;
        else if (cancel && cancel->isCancelled()) ctx.TryCancel();   // TIMEOUT
    }
    cq.Shutdown();
    { void* t; bool o; while (cq.Next(&t, &o)) {} }   // drain
}

// The async machinery for one generic streaming call (bundled to keep helper signatures small).
struct GrpcCall {
    GenericRW& rw;
    grpc::CompletionQueue& cq;
    grpc::ClientContext& ctx;
    const std::shared_ptr<CancelToken>& cancel;
};

using EmitFn = std::function<bool(const grpc::ByteBuffer&)>;

// Client-streaming wire dance: StartCall, Write each message, WritesDone, Read ONE response, Finish.
// Fills respBuf/gotResp/status; shuts down + drains the CQ.
void runClientStreamCall(GrpcCall& call, const std::vector<std::string>& wire,
                         grpc::ByteBuffer& respBuf, bool& gotResp, grpc::Status& status) {
    void* tag = reinterpret_cast<void*>(1);
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
    call.cq.Shutdown();
    { void* t; bool o; while (call.cq.Next(&t, &o)) {} }   // drain
}

// Server-streaming read loop: write the single request, half-close, then read until the server closes
// (or a ceiling is hit — `emit` returns false). Drives the CQ via pumpCq; cancel -> TryCancel.
void runServerStreamReads(GrpcCall& call, const std::string& reqWire, const EmitFn& emit) {
    void* kTag = reinterpret_cast<void*>(1);
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
        if (pumpCq(call.cq, call.cancel, call.ctx) != 1) break;   // !ok -> server closed (or CQ shutdown)
        if (!emit(msg)) break;
    }
}

// --- Bidi dispatch state + per-tag handlers (extracted so the loop stays flat). ---
// Three distinct tags so write/read/writes-done completions never alias each other (M9).
void* bidiTag(int n) { return reinterpret_cast<void*>(static_cast<std::intptr_t>(n)); }

struct BidiPump {
    GenericRW& rw;
    const std::vector<std::string>& wire;
    grpc::ByteBuffer writeBuf;
    grpc::ByteBuffer readBuf;
    size_t widx = 0;
    enum WState { WRITING, WDONE_PENDING, WDONE_DONE };
    WState wstate = WRITING; // M9: never read uninitialized
    bool reading = true;
};

void bidiStartWrite(BidiPump& p, size_t idx) {
    grpc::Slice sl(p.wire[idx]);
    p.writeBuf = grpc::ByteBuffer(&sl, 1);
    p.rw.Write(p.writeBuf, bidiTag(1));
}

// A write completed: advance to the next message, or half-close when the last one is out.
void bidiOnWrite(BidiPump& p, bool ok) {
    if (!ok) { p.wstate = BidiPump::WDONE_DONE; return; }     // write side broke -> stop writing
    if (++p.widx < p.wire.size()) { bidiStartWrite(p, p.widx); return; }
    p.rw.WritesDone(bidiTag(3));
    p.wstate = BidiPump::WDONE_PENDING;
}

// A read completed: emit it and queue the next read, or stop reading on end/cancel/ceiling.
void bidiOnRead(BidiPump& p, bool ok, GrpcCall& call, const EmitFn& emit) {
    if (!ok) { p.reading = false; return; }                  // server finished its side
    if (call.cancel && call.cancel->cancelled()) { call.ctx.TryCancel(); p.reading = false; return; }
    if (!emit(p.readBuf)) { p.reading = false; return; }     // ceiling hit (TryCancel already done)
    p.rw.Read(&p.readBuf, bidiTag(2));                        // keep reading
}

// Bidi dispatch loop: keep ONE write-side op and ONE read-side op outstanding, dispatch by tag so writes
// and reads interleave freely (avoids the write-all-then-read deadlock on ping-pong RPCs).
void runBidiStream(GrpcCall& call, const std::vector<std::string>& wire, const EmitFn& emit) {
    BidiPump p{call.rw, wire};
    if (!wire.empty()) bidiStartWrite(p, 0);
    else { call.rw.WritesDone(bidiTag(3)); p.wstate = BidiPump::WDONE_PENDING; }
    call.rw.Read(&p.readBuf, bidiTag(2));

    while (p.wstate != BidiPump::WDONE_DONE || p.reading) {
        void* tag = nullptr;
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

// Decode one streamed protobuf message into a compact JSON element; on failure return a VALID error
// element (keeps the array valid, §10). Reuses `outMsg` (Clear() per call) to avoid per-event allocs.
std::string decodeStreamMessage(gp::Message& outMsg, const grpc::ByteBuffer& msg, std::uint64_t seq) {
    outMsg.Clear();
    bool decoded = outMsg.ParseFromString(byteBufferToString(msg));
    std::string json;
    if (decoded) {
        gp::util::JsonPrintOptions jopts;
        jopts.add_whitespace = false;   // compact per element; the UI lays out the array
        jopts.always_print_fields_with_no_presence = true;
        decoded = gp::util::MessageToJsonString(outMsg, &json, jopts).ok();
    }
    if (!decoded)
        json = "{\"__decode_error__\":\"failed to decode protobuf message\",\"seq\":" +
               std::to_string(seq) + "}";
    return json;
}

// Accumulates streamed events: decode -> emit as a StreamEvent -> enforce the §9 event/byte ceilings.
struct StreamEmitter {
    IStreamSink& sink;
    grpc::ClientContext& ctx;
    gp::Message& outMsg;
    std::uint64_t maxEvents;
    std::uint64_t maxBytes;
    std::function<long long()> offsetMs;
    std::uint64_t seq = 0;
    std::uint64_t bytes = 0;
    bool truncated = false;

    // Returns false once a ceiling is hit (TryCancel already issued) so the caller stops reading.
    bool emit(const grpc::ByteBuffer& msg) {
        std::string json = decodeStreamMessage(outMsg, msg, seq);
        StreamEvent ev;
        ev.seq = seq;
        ev.kind = StreamPayloadKind::Json;
        ev.payload = json;
        ev.name = "message";
        ev.offsetMs = offsetMs();
        sink.onStreamEvent(ev);
        ++seq;
        bytes += json.size();
        if (seq >= maxEvents || bytes >= maxBytes) { truncated = true; ctx.TryCancel(); return false; }
        return true;
    }
};

// Serialize the request message(s) to wire bytes up front (so a bad message fails before opening the call).
// server-streaming: exactly one (object); bidi: a JSON ARRAY (single object = one). false + err on failure.
bool serializeStreamRequests(const GrpcRequest& g, const gp::MethodDescriptor* mth, bool isBidi,
                             gp::DynamicMessageFactory& factory, std::vector<std::string>& wire,
                             std::string& err) {
    if (isBidi) {
        std::vector<std::string> jsons;
        return splitMessages(g.message, jsons, err) &&
               serializeMessages(jsons, mth->input_type(), factory, wire, err);
    }
    std::string b;
    if (!buildRequestBytes(factory, mth, g.message, b, err)) return false;
    wire.push_back(std::move(b));
    return true;
}

// StartCall, then drive the appropriate read/write loop once the call is live. M9: Start uses tag 10,
// outside the run* helpers' in-loop tag space (1/2/3), so a delayed StartCall can't be misread as a write.
void driveStream(GrpcCall& call, bool isBidi, const std::vector<std::string>& wire, const EmitFn& emit) {
    void* kStartTag = reinterpret_cast<void*>(10);
    call.rw.StartCall(kStartTag);
    if (pumpCq(call.cq, call.cancel, call.ctx) != 1) return;   // call never became live
    if (isBidi) runBidiStream(call, wire, emit);
    else runServerStreamReads(call, wire[0], emit);
}

// Finish the call (tag 11, distinct from the loop tags), pump it, then shut down + drain the CQ.
void finishStreamCall(GrpcCall& call, grpc::Status& status) {
    void* kFinishTag = reinterpret_cast<void*>(11);
    call.rw.Finish(&status, kFinishTag);
    pumpCq(call.cq, call.cancel, call.ctx);
    call.cq.Shutdown();
    { void* t; bool o; while (call.cq.Next(&t, &o)) {} }   // drain remaining tags
}

// Build + emit the terminal StreamEnd from the final status and the emitter's counters (§3 close contract).
void closeStream(IStreamSink& sink, grpc::ClientContext& ctx, const grpc::Status& status,
                 const StreamEmitter& em, bool cancelled, long long elapsedMs) {
    StreamEnd end;
    end.status = mapStreamStatus(status, cancelled && !em.truncated);
    end.statusCode = status.error_code();
    end.statusMessage = status.error_message();
    end.trailing = trailingToKv(ctx.GetServerTrailingMetadata());
    end.totalEvents = em.seq;
    end.totalBytes = em.bytes;
    end.elapsedMs = elapsedMs;
    end.truncated = em.truncated;
    // Truncation is a successful cap-hit, not a failure: report Ok so the array stays valid (§10).
    if (em.truncated && end.status != StreamStatus::Cancelled) end.status = StreamStatus::Ok;
    sink.onStreamClose(end);
}

// Client-streaming: write N request messages, half-close, read ONE response (SPEC §0 — v2 scope).
// `message` is a JSON array of request messages (single object = one message; empty = zero). Result is
// unary-shaped -> delivered via onResponse/onError, so the UI/CLI need no new receive path.
void sendClientStream(const ResolvedRequest& req, const gp::MethodDescriptor* mth,
                      const gp::DescriptorPool* pool, RequestHandle handle,
                      IUiDelegate& delegate, const std::shared_ptr<CancelToken>& cancel) {
    const GrpcRequest& g = req.model.grpc;

    // 1+2. Split the message field (array | single | empty) and serialize each up front (fail fast).
    std::vector<std::string> msgJsons, wire;
    std::string err;
    if (!splitMessages(g.message, msgJsons, err)) {
        delegate.onError(handle, ApiError{ErrorKind::Parse, err});
        return;
    }
    gp::DynamicMessageFactory factory(pool);
    if (!serializeMessages(msgJsons, mth->input_type(), factory, wire, err)) {
        delegate.onError(handle, ApiError{ErrorKind::Parse, err});
        return;
    }

    // 3. Channel + generic stub + context (metadata + deadline).
    auto channel = grpc::CreateCustomChannel(g.target, makeCreds(g.tls), streamChannelArgs());
    grpc::GenericStub stub(channel);
    grpc::ClientContext clientCtx;
    applyCallContext(clientCtx, g);

    const std::string methodPath = "/" + g.service + "/" + g.method;
    grpc::CompletionQueue cq;
    std::unique_ptr<GenericRW> rw = stub.PrepareCall(&clientCtx, methodPath, &cq);
    GrpcCall call{*rw, cq, clientCtx, cancel};

    // 4+5. Write N messages, half-close, read ONE response, Finish.
    const auto start = std::chrono::steady_clock::now();
    grpc::ByteBuffer respBuf;
    bool gotResp = false;
    grpc::Status status;
    runClientStreamCall(call, wire, respBuf, gotResp, status);

    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start).count();

    if (cancel && cancel->cancelled()) {
        delegate.onError(handle, ApiError{ErrorKind::Cancelled, "Cancelled"});
        return;
    }
    if (!status.ok()) {
        delegate.onError(handle, ApiError{mapGrpcStatus(status.error_code()),
                                          "gRPC " + std::to_string(status.error_code()) + ": " +
                                              status.error_message()});
        return;
    }

    // 6. Response message -> JSON.
    std::string jsonOut;
    if (gotResp && !decodeUnaryResponse(factory, mth, respBuf, jsonOut, err)) {
        delegate.onError(handle, ApiError{ErrorKind::Parse, err});
        return;
    }
    deliverUnaryOk(delegate, handle, req.model, jsonOut, static_cast<long>(elapsed));
}

} // namespace

void GrpcSender::send(const ResolvedRequest& req, RequestHandle handle, IUiDelegate& delegate,
                      const std::shared_ptr<CancelToken>& cancel) {
    const GrpcRequest& g = req.model.grpc;

    // Load descriptors (protoFiles | descriptorSet | reflection) + find the method.
    DescriptorContext ctx;
    std::string err;
    const gp::MethodDescriptor* mth = resolveMethod(g, ctx, err);
    if (!mth) {
        delegate.onError(handle, ApiError{ErrorKind::Parse, err});
        return;
    }
    // Server-streaming has its own receive path (Engine::openStream / IStreamSink); not on send().
    if (mth->server_streaming()) {
        delegate.onError(handle, ApiError{ErrorKind::Unsupported,
                                          "server-streaming uses the stream path, not send()."});
        return;
    }
    // Client-streaming: N request messages -> ONE response. Unary-shaped result, so it rides send()
    // and reports via onResponse/onError. The `message` field is a JSON ARRAY of request messages
    // (a single object is also accepted = one message; empty = zero messages).
    if (mth->client_streaming()) {
        sendClientStream(req, mth, ctx.activePool, handle, delegate, cancel);
        return;
    }

    // JSON message -> protobuf wire bytes.
    gp::DynamicMessageFactory factory(ctx.activePool);
    std::string reqBytes;
    if (!buildRequestBytes(factory, mth, g.message, reqBytes, err)) {
        delegate.onError(handle, ApiError{ErrorKind::Parse, err});
        return;
    }

    // Channel + generic stub + context.
    auto channel = grpc::CreateCustomChannel(g.target, makeCreds(g.tls), streamChannelArgs());
    grpc::GenericStub stub(channel);
    grpc::ClientContext clientCtx;
    applyCallContext(clientCtx, g);

    grpc::Slice reqSlice(reqBytes);
    grpc::ByteBuffer reqBuffer(&reqSlice, 1);
    std::string methodPath = "/" + g.service + "/" + g.method;

    grpc::CompletionQueue cq;
    auto start = std::chrono::steady_clock::now();
    auto reader = stub.PrepareUnaryCall(&clientCtx, methodPath, reqBuffer, &cq);
    reader->StartCall();

    grpc::ByteBuffer respBuffer;
    grpc::Status status;
    void* finishTag = reinterpret_cast<void*>(1);
    reader->Finish(&respBuffer, &status, finishTag);
    awaitUnaryFinish(cq, clientCtx, cancel);   // poll CQ + honor cancel, then shutdown + drain

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start).count();

    if (cancel && cancel->isCancelled()) {
        delegate.onError(handle, ApiError{ErrorKind::Cancelled, "Cancelled"});
        return;
    }
    if (!status.ok()) {
        delegate.onError(handle, ApiError{mapGrpcStatus(status.error_code()),
                                          "gRPC " + std::to_string(status.error_code()) + ": " +
                                              status.error_message()});
        return;
    }

    // protobuf response -> JSON.
    std::string jsonOut;
    if (!decodeUnaryResponse(factory, mth, respBuffer, jsonOut, err)) {
        delegate.onError(handle, ApiError{ErrorKind::Parse, err});
        return;
    }
    deliverUnaryOk(delegate, handle, req.model, jsonOut, static_cast<long>(elapsed));
}

bool GrpcSender::isStreaming(const ResolvedRequest& req) const {
    return req.model.grpc.methodType == "server_streaming";
}

// SPEC_grpc_streaming §5.2: generic stub only exposes the ASYNC API for streaming, so we drive the
// CompletionQueue ourselves: PrepareCall -> StartCall -> Write(request) -> WritesDone -> Read* -> Finish.
// Reads run on this (background) thread; events flow into `sink` in seq order; cancel -> TryCancel.
void GrpcSender::openStream(const ResolvedRequest& req, IStreamSink& sink,
                            const std::shared_ptr<CancelToken>& cancel) {
    // Configured ceilings (§9): from EngineConfig (.env) via ResolvedRequest; 0 -> built-in default.
    const std::uint64_t kMaxEvents = req.streamMaxEvents ? req.streamMaxEvents : 100000;
    const std::uint64_t kMaxBytes = req.streamMaxBytes ? req.streamMaxBytes : 64ull * 1024 * 1024;

    const GrpcRequest& g = req.model.grpc;
    const auto t0 = std::chrono::steady_clock::now();
    auto offsetMs = [&] {
        return static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count());
    };

    StreamMeta meta;
    meta.streamId = req.streamId;
    meta.transport = StreamTransport::Grpc;
    meta.startedAtEpochMs = nowEpochMs();

    // open() must precede close() always; this helper emits open once then closes with an error.
    auto openThenError = [&](const std::string& msg) {
        sink.onStreamOpen(meta);
        StreamEnd end;
        end.status = StreamStatus::Error;
        end.statusMessage = msg;
        end.elapsedMs = offsetMs();
        sink.onStreamClose(end);
    };

    // Descriptors (reflection | protoFiles | descriptorSet) + method — same path as unary.
    DescriptorContext dctx;
    std::string err;
    const gp::MethodDescriptor* mth = resolveMethod(g, dctx, err);
    if (!mth) { openThenError(err); return; }
    // openStream serves methods that STREAM responses: server-streaming (1 request) and bidi (N requests).
    const bool isBidi = mth->client_streaming();
    if (!mth->server_streaming()) {
        openThenError("not a server-streaming/bidi method: " + g.service + "/" + g.method);
        return;
    }

    // Request message(s) -> wire bytes (serialize up front so a bad message fails before opening the call).
    gp::DynamicMessageFactory factory(dctx.activePool);
    std::vector<std::string> wire;
    if (!serializeStreamRequests(g, mth, isBidi, factory, wire, err)) { openThenError(err); return; }

    // Channel + generic stub + context. M10: for a stream the deadline is a MAX-DURATION cap from the
    // per-request Config (timeout_ms, default 30 min), not a hard 30s — cancel + the §9 ceilings also bound it.
    auto channel = grpc::CreateCustomChannel(g.target, makeCreds(g.tls), streamChannelArgs());
    grpc::GenericStub stub(channel);
    grpc::ClientContext ctx;
    applyCallContext(ctx, g);

    const std::string methodPath = "/" + g.service + "/" + g.method;
    grpc::CompletionQueue cq;
    std::unique_ptr<GenericRW> rw = stub.PrepareCall(&ctx, methodPath, &cq);

    // From here on the §3 contract is live: exactly one open, then events, then exactly one close.
    sink.onStreamOpen(meta);

    // Reuse ONE response message across the whole stream (Clear() per event) instead of allocating a new
    // DynamicMessage each time — cuts allocator churn on high-frequency streams (perf spec §2.8).
    std::unique_ptr<gp::Message> outMsg(factory.GetPrototype(mth->output_type())->New());
    StreamEmitter emitter{sink, ctx, *outMsg, kMaxEvents, kMaxBytes, offsetMs};
    auto emit = [&](const grpc::ByteBuffer& m) { return emitter.emit(m); };

    GrpcCall call{*rw, cq, ctx, cancel};
    driveStream(call, isBidi, wire, emit);   // StartCall + the server-streaming/bidi read-write loop

    grpc::Status status;
    finishStreamCall(call, status);          // Finish -> trailing status + metadata; shutdown + drain
    closeStream(sink, ctx, status, emitter, cancel && cancel->cancelled(), offsetMs());
}

} // namespace core

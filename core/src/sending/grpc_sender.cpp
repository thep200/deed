#include "sending/grpc_sender.hpp"

#include <chrono>
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
#include <grpcpp/support/byte_buffer.h>
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
    auto channel = grpc::CreateChannel(g.target, makeCreds(g.tls));
    grpc::GenericStub stub(channel);
    grpc::ClientContext clientCtx;
    int deadlineMs = g.settings.deadlineMs > 0 ? g.settings.deadlineMs : 30000;
    clientCtx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(deadlineMs));
    for (const auto& md : g.metadata)
        if (md.enabled && !md.key.empty()) clientCtx.AddMetadata(md.key, md.value);

    const std::string methodPath = "/" + g.service + "/" + g.method;
    grpc::CompletionQueue cq;
    std::unique_ptr<GenericRW> rw = stub.PrepareCall(&clientCtx, methodPath, &cq);

    const auto start = std::chrono::steady_clock::now();
    void* tag = reinterpret_cast<void*>(1);

    // 4. StartCall -> Write each message -> WritesDone (half-close).
    rw->StartCall(tag);
    bool failed = (pumpCq(cq, cancel, clientCtx) != 1);
    for (size_t i = 0; !failed && i < wire.size(); ++i) {
        if (cancel && cancel->cancelled()) break;
        grpc::Slice sl(wire[i]);
        grpc::ByteBuffer buf(&sl, 1);
        rw->Write(buf, tag);
        if (pumpCq(cq, cancel, clientCtx) != 1) { failed = true; break; }
    }
    if (!failed) { rw->WritesDone(tag); pumpCq(cq, cancel, clientCtx); }

    // 5. Read the single response, then Finish.
    grpc::ByteBuffer respBuf;
    bool gotResp = false;
    if (!failed) {
        rw->Read(&respBuf, tag);
        gotResp = (pumpCq(cq, cancel, clientCtx) == 1);
    }
    grpc::Status status;
    rw->Finish(&status, tag);
    pumpCq(cq, cancel, clientCtx);
    cq.Shutdown();
    { void* t; bool o; while (cq.Next(&t, &o)) {} }   // drain

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
    if (gotResp) {
        std::unique_ptr<gp::Message> respMsg(factory.GetPrototype(mth->output_type())->New());
        std::string respBytes = byteBufferToString(respBuf);
        if (!respMsg->ParseFromString(respBytes)) {
            delegate.onError(handle, ApiError{ErrorKind::Parse, "failed to parse protobuf response"});
            return;
        }
        gp::util::JsonPrintOptions jopts;
        jopts.add_whitespace = true;
        jopts.always_print_fields_with_no_presence = true;
        auto st = gp::util::MessageToJsonString(*respMsg, &jsonOut, jopts);
        if (!st.ok()) {
            delegate.onError(handle, ApiError{ErrorKind::Parse,
                                              "failed to convert response to JSON: " +
                                                  std::string(st.message())});
            return;
        }
    }

    ApiResponse resp;
    resp.statusCode = 0;   // gRPC OK
    resp.statusText = "OK";
    resp.body = jsonOut;
    resp.elapsedMs = static_cast<long>(elapsed);
    resp.sizeBytes = static_cast<std::int64_t>(jsonOut.size());
    resp.resolvedRequestDump = codec::dumpRequest(req.model);
    delegate.onResponse(handle, resp);
}

} // namespace

void GrpcSender::send(const ResolvedRequest& req, RequestHandle handle, IUiDelegate& delegate,
                      const std::shared_ptr<CancelToken>& cancel) {
    const GrpcRequest& g = req.model.grpc;

    // Load descriptors (protoFiles | descriptorSet | reflection).
    DescriptorContext ctx;
    if (!grpcdesc::buildDescriptors(g, ctx)) {
        delegate.onError(handle, ApiError{ErrorKind::Parse, ctx.error});
        return;
    }

    // Find service + method.
    const gp::ServiceDescriptor* svc = ctx.activePool->FindServiceByName(g.service);
    if (!svc) {
        delegate.onError(handle, ApiError{ErrorKind::Parse, "service not found: " + g.service});
        return;
    }
    const gp::MethodDescriptor* mth = svc->FindMethodByName(g.method);
    if (!mth) {
        delegate.onError(handle, ApiError{ErrorKind::Parse, "method not found: " + g.method});
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

    // JSON message -> protobuf (DynamicMessage).
    gp::DynamicMessageFactory factory(ctx.activePool);
    std::unique_ptr<gp::Message> reqMsg(factory.GetPrototype(mth->input_type())->New());
    {
        gp::util::JsonParseOptions popts;
        popts.ignore_unknown_fields = true;
        auto st = gp::util::JsonStringToMessage(g.message.empty() ? "{}" : g.message, reqMsg.get(), popts);
        if (!st.ok()) {
            delegate.onError(handle, ApiError{ErrorKind::Parse,
                                              "invalid JSON message for " +
                                                  std::string(mth->input_type()->full_name()) + ": " +
                                                  std::string(st.message())});
            return;
        }
    }
    std::string reqBytes;
    if (!reqMsg->SerializeToString(&reqBytes)) {
        delegate.onError(handle, ApiError{ErrorKind::Parse, "failed to serialize protobuf request"});
        return;
    }

    // Channel + generic stub.
    auto channel = grpc::CreateChannel(g.target, makeCreds(g.tls));
    grpc::GenericStub stub(channel);

    grpc::ClientContext clientCtx;
    int deadlineMs = g.settings.deadlineMs > 0 ? g.settings.deadlineMs : 30000;
    clientCtx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(deadlineMs));
    for (const auto& md : g.metadata) {
        if (md.enabled && !md.key.empty()) clientCtx.AddMetadata(md.key, md.value);
    }

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

    // Poll CQ + support cancel.
    bool done = false;
    while (!done) {
        void* tag = nullptr;
        bool ok = false;
        auto deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(100);
        auto next = cq.AsyncNext(&tag, &ok, deadline);
        if (next == grpc::CompletionQueue::GOT_EVENT) {
            done = true;
        } else if (next == grpc::CompletionQueue::SHUTDOWN) {
            break;
        } else { // TIMEOUT
            if (cancel && cancel->isCancelled()) clientCtx.TryCancel();
        }
    }
    cq.Shutdown();
    { void* t; bool o; while (cq.Next(&t, &o)) {} } // drain

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
    std::unique_ptr<gp::Message> respMsg(factory.GetPrototype(mth->output_type())->New());
    std::string respBytes = byteBufferToString(respBuffer);
    if (!respMsg->ParseFromString(respBytes)) {
        delegate.onError(handle, ApiError{ErrorKind::Parse, "failed to parse protobuf response"});
        return;
    }
    std::string jsonOut;
    {
        gp::util::JsonPrintOptions jopts;
        jopts.add_whitespace = true;
        jopts.always_print_fields_with_no_presence = true;
        auto st = gp::util::MessageToJsonString(*respMsg, &jsonOut, jopts);
        if (!st.ok()) {
            delegate.onError(handle, ApiError{ErrorKind::Parse,
                                              "failed to convert response to JSON: " +
                                                  std::string(st.message())});
            return;
        }
    }

    ApiResponse resp;
    resp.statusCode = 0; // gRPC OK
    resp.statusText = "OK";
    resp.body = jsonOut;
    resp.elapsedMs = static_cast<long>(elapsed);
    resp.sizeBytes = static_cast<std::int64_t>(jsonOut.size());
    resp.resolvedRequestDump = codec::dumpRequest(req.model);
    delegate.onResponse(handle, resp);
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

    // Descriptors (reflection | protoFiles | descriptorSet) — same path as unary.
    DescriptorContext dctx;
    if (!grpcdesc::buildDescriptors(g, dctx)) { openThenError(dctx.error); return; }
    const gp::ServiceDescriptor* svc = dctx.activePool->FindServiceByName(g.service);
    if (!svc) { openThenError("service not found: " + g.service); return; }
    const gp::MethodDescriptor* mth = svc->FindMethodByName(g.method);
    if (!mth) { openThenError("method not found: " + g.method); return; }
    // openStream serves methods that STREAM responses: server-streaming (1 request) and bidi (N requests).
    const bool isBidi = mth->client_streaming();
    if (!mth->server_streaming()) {
        openThenError("not a server-streaming/bidi method: " + g.service + "/" + g.method);
        return;
    }

    // Request message(s) -> wire bytes (serialize up front so a bad message fails before opening the call).
    // server-streaming: exactly one (object); bidi: a JSON ARRAY of messages (single object = one).
    gp::DynamicMessageFactory factory(dctx.activePool);
    std::vector<std::string> wire;
    {
        std::string err;
        if (isBidi) {
            std::vector<std::string> jsons;
            if (!splitMessages(g.message, jsons, err) ||
                !serializeMessages(jsons, mth->input_type(), factory, wire, err)) {
                openThenError(err);
                return;
            }
        } else {
            std::unique_ptr<gp::Message> reqMsg(factory.GetPrototype(mth->input_type())->New());
            gp::util::JsonParseOptions popts;
            popts.ignore_unknown_fields = true;
            auto st = gp::util::JsonStringToMessage(g.message.empty() ? "{}" : g.message, reqMsg.get(), popts);
            if (!st.ok()) {
                openThenError("invalid JSON message for " + std::string(mth->input_type()->full_name()) +
                              ": " + std::string(st.message()));
                return;
            }
            std::string b;
            if (!reqMsg->SerializeToString(&b)) { openThenError("failed to serialize protobuf request"); return; }
            wire.push_back(std::move(b));
        }
    }

    // Channel + generic stub + context (metadata + deadline).
    auto channel = grpc::CreateChannel(g.target, makeCreds(g.tls));
    grpc::GenericStub stub(channel);
    grpc::ClientContext ctx;
    int deadlineMs = g.settings.deadlineMs > 0 ? g.settings.deadlineMs : 30000;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(deadlineMs));
    for (const auto& md : g.metadata)
        if (md.enabled && !md.key.empty()) ctx.AddMetadata(md.key, md.value);

    const std::string methodPath = "/" + g.service + "/" + g.method;
    grpc::CompletionQueue cq;
    std::unique_ptr<GenericRW> rw = stub.PrepareCall(&ctx, methodPath, &cq);

    // From here on the §3 contract is live: exactly one open, then events, then exactly one close.
    sink.onStreamOpen(meta);

    std::uint64_t seq = 0, bytes = 0;
    bool truncated = false;

    auto makeBuf = [](const std::string& s) { grpc::Slice sl(s); return grpc::ByteBuffer(&sl, 1); };

    // Decode one response message -> JSON, emit as a StreamEvent. Returns false if a ceiling was hit
    // (caller stops reading). On decode failure emits a VALID error element (keeps the array valid, §10).
    auto emitMessage = [&](const grpc::ByteBuffer& msg) -> bool {
        std::string json;
        std::unique_ptr<gp::Message> outMsg(factory.GetPrototype(mth->output_type())->New());
        std::string raw = byteBufferToString(msg);
        bool decoded = outMsg->ParseFromString(raw);
        if (decoded) {
            gp::util::JsonPrintOptions jopts;
            jopts.add_whitespace = false;   // compact per element; the UI lays out the array
            jopts.always_print_fields_with_no_presence = true;
            decoded = gp::util::MessageToJsonString(*outMsg, &json, jopts).ok();
        }
        if (!decoded)
            json = "{\"__decode_error__\":\"failed to decode protobuf message\",\"seq\":" +
                   std::to_string(seq) + "}";
        StreamEvent ev;
        ev.seq = seq;
        ev.kind = StreamPayloadKind::Json;
        ev.payload = json;
        ev.name = "message";
        ev.offsetMs = offsetMs();
        sink.onStreamEvent(ev);
        ++seq;
        bytes += json.size();
        if (seq >= kMaxEvents || bytes >= kMaxBytes) { truncated = true; ctx.TryCancel(); return false; }
        return true;
    };

    void* kTag = reinterpret_cast<void*>(1);
    bool startupFailed = false;
    rw->StartCall(kTag);
    if (pumpCq(cq, cancel, ctx) != 1) startupFailed = true;

    if (!startupFailed && !isBidi) {
        // --- Server-streaming: write the single request, half-close, then read until the server closes.
        grpc::ByteBuffer reqBuffer = makeBuf(wire[0]);
        rw->Write(reqBuffer, kTag);
        if (pumpCq(cq, cancel, ctx) != 1) startupFailed = true;
        if (!startupFailed) { rw->WritesDone(kTag); if (pumpCq(cq, cancel, ctx) != 1) startupFailed = true; }
        while (!startupFailed) {
            if (cancel && cancel->cancelled()) { ctx.TryCancel(); break; }
            grpc::ByteBuffer msg;
            rw->Read(&msg, kTag);
            if (pumpCq(cq, cancel, ctx) != 1) break;   // !ok -> server closed the stream (or CQ shutdown)
            if (!emitMessage(msg)) break;
        }
    } else if (!startupFailed && isBidi) {
        // --- Bidi: keep ONE write-side op and ONE read-side op outstanding at once, dispatch by tag.
        // Writes and reads interleave freely (avoids the deadlock of write-all-then-read on ping-pong RPCs).
        void* T_WRITE = reinterpret_cast<void*>(1);
        void* T_READ = reinterpret_cast<void*>(2);
        void* T_WDONE = reinterpret_cast<void*>(3);
        grpc::ByteBuffer writeBuf, readBuf;
        size_t widx = 0;
        enum WState { WRITING, WDONE_PENDING, WDONE_DONE } wstate;

        if (!wire.empty()) { writeBuf = makeBuf(wire[widx]); rw->Write(writeBuf, T_WRITE); wstate = WRITING; }
        else { rw->WritesDone(T_WDONE); wstate = WDONE_PENDING; }
        bool reading = true;
        rw->Read(&readBuf, T_READ);

        while (wstate != WDONE_DONE || reading) {
            void* tag = nullptr;
            bool ok = false;
            auto deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(100);
            auto st = cq.AsyncNext(&tag, &ok, deadline);
            if (st == grpc::CompletionQueue::SHUTDOWN) break;
            if (st == grpc::CompletionQueue::TIMEOUT) {
                if (cancel && cancel->cancelled()) ctx.TryCancel();
                continue;
            }
            // GOT_EVENT
            if (tag == T_WRITE) {
                if (!ok) { wstate = WDONE_DONE; }                       // write side broke -> stop writing
                else if (++widx < wire.size()) { writeBuf = makeBuf(wire[widx]); rw->Write(writeBuf, T_WRITE); }
                else { rw->WritesDone(T_WDONE); wstate = WDONE_PENDING; }
            } else if (tag == T_WDONE) {
                wstate = WDONE_DONE;
            } else if (tag == T_READ) {
                if (!ok) { reading = false; }                          // server finished its side
                else if (cancel && cancel->cancelled()) { ctx.TryCancel(); reading = false; }
                else if (!emitMessage(readBuf)) { reading = false; }    // ceiling hit (TryCancel already done)
                else { rw->Read(&readBuf, T_READ); }                    // keep reading
            }
            if (cancel && cancel->cancelled()) ctx.TryCancel();
        }
    }

    // Finish -> trailing status + metadata.
    grpc::Status status;
    rw->Finish(&status, kTag);
    pumpCq(cq, cancel, ctx);
    cq.Shutdown();
    { void* t; bool o; while (cq.Next(&t, &o)) {} }   // drain remaining tags

    const bool wasCancelled = cancel && cancel->cancelled() && !truncated;
    StreamEnd end;
    end.status = mapStreamStatus(status, wasCancelled);
    end.statusCode = status.error_code();
    end.statusMessage = status.error_message();
    end.trailing = trailingToKv(ctx.GetServerTrailingMetadata());
    end.totalEvents = seq;
    end.totalBytes = bytes;
    end.elapsedMs = offsetMs();
    end.truncated = truncated;
    // Truncation is a successful cap-hit, not a failure: report Ok so the array stays valid (§10).
    if (truncated && end.status != StreamStatus::Cancelled) end.status = StreamStatus::Ok;
    sink.onStreamClose(end);
}

} // namespace core

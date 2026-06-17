#include "sending/grpc_sender.hpp"

#include <chrono>
#include <memory>
#include <string>

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

} // namespace

void GrpcSender::send(const ResolvedRequest& req, RequestHandle handle, IUiDelegate& delegate,
                      const std::shared_ptr<CancelToken>& cancel) {
    const GrpcRequest& g = req.model.grpc;

    // POC: chỉ unary.
    if (g.methodType != "unary") {
        delegate.onError(handle, ApiError{ErrorKind::Unsupported,
                                          "POC supports unary only; methodType '" + g.methodType +
                                              "' cannot be sent."});
        return;
    }

    // Nạp descriptor (protoFiles | descriptorSet | reflection).
    DescriptorContext ctx;
    if (!grpcdesc::buildDescriptors(g, ctx)) {
        delegate.onError(handle, ApiError{ErrorKind::Parse, ctx.error});
        return;
    }

    // Tìm service + method.
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
    if (mth->client_streaming() || mth->server_streaming()) {
        delegate.onError(handle, ApiError{ErrorKind::Unsupported,
                                          "this method is streaming — POC runs unary only."});
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

    // Kênh + generic stub.
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

    // Poll CQ + hỗ trợ cancel.
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

} // namespace core

// core/i_request_sender.hpp — Protocol abstraction (README §8.1).
// HTTP and gRPC share the send path; a new protocol = a new sender, Engine untouched.
#pragma once

#include <memory>

#include "core/i_ui_delegate.hpp"
#include "core/sending/cancel_token.hpp"
#include "core/streaming/i_stream_sink.hpp"
#include "core/types.hpp"

namespace core {

class IRequestSender {
public:
    virtual ~IRequestSender() = default;

    // Send a RESOLVED request (UNARY). Called from a background thread (Engine already dispatched to a
    // pool). Sender is responsible for calling exactly ONE terminal callback on the delegate.
    virtual void send(const ResolvedRequest& req,
                      RequestHandle handle,
                      IUiDelegate& delegate,
                      const std::shared_ptr<CancelToken>& cancel) = 0;

    // Does this resolved request map to a server-streaming call? Engine routes accordingly.
    virtual bool isStreaming(const ResolvedRequest&) const { return false; }

    // Open a SERVER-STREAM. Runs synchronously on the background thread Engine provides; pushes events
    // into `sink` honoring the §3 contract; respects cancel. Default: report "not supported" + close.
    virtual void openStream(const ResolvedRequest& req,
                            IStreamSink& sink,
                            const std::shared_ptr<CancelToken>& cancel) {
        (void)req; (void)cancel;
        sink.onStreamOpen(StreamMeta{req.streamId, StreamTransport::Grpc, {}, 0});
        StreamEnd end;
        end.status = StreamStatus::Error;
        end.statusMessage = "streaming not supported by this sender";
        sink.onStreamClose(end);
    }
};

} // namespace core

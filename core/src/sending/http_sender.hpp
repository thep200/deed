// http_sender.hpp — HttpSender (cpr/libcurl). README §7.2, §8.1.
#pragma once

#include "core/sending/i_request_sender.hpp"

namespace core {

class HttpSender : public IRequestSender {
public:
    void send(const ResolvedRequest& req, RequestHandle handle, IUiDelegate& delegate,
              const std::shared_ptr<CancelToken>& cancel) override;

    // SSE (SPEC_sse): a streaming consumption mode of HTTP. Routes here when streamMode is Sse/Auto.
    bool isStreaming(const ResolvedRequest& req) const override;
    void openStream(const ResolvedRequest& req, IStreamSink& sink,
                    const std::shared_ptr<CancelToken>& cancel) override;
};

} // namespace core

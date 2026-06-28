// graphql_sender.hpp — GraphQL sender (SPEC_graphql §3/§6). INTERNAL (core/src).
// GraphQL adds no transport of its own: query/mutation are repackaged as HTTP POST (delegated to
// HttpSender); a subscription opens a WebSocket and runs the graphql-transport-ws / legacy graphql-ws
// protocol (GraphQlWsProtocol) on top, translating its envelope into clean StreamEvents.
#pragma once

#include <memory>

#include "core/sending/i_request_sender.hpp"
#include "sending/http_sender.hpp"

namespace core {

class GraphQlSender : public IRequestSender {
public:
    void send(const ResolvedRequest& req, RequestHandle handle, IUiDelegate& delegate,
              const std::shared_ptr<CancelToken>& cancel) override;
    bool isStreaming(const ResolvedRequest& req) const override;
    void openStream(const ResolvedRequest& req, IStreamSink& sink,
                    const std::shared_ptr<CancelToken>& cancel) override;

private:
    HttpSender http_;   // query/mutation delegate (GraphQL-over-HTTP)
};

} // namespace core

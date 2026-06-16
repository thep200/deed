// http_sender.hpp — HttpSender (cpr/libcurl). README §7.2, §8.1.
#pragma once

#include "core/i_request_sender.hpp"

namespace core {

class HttpSender : public IRequestSender {
public:
    void send(const ResolvedRequest& req, RequestHandle handle, IUiDelegate& delegate,
              const std::shared_ptr<CancelToken>& cancel) override;
};

} // namespace core

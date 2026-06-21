// core/i_request_sender.hpp — Protocol abstraction (README §8.1).
// HTTP and gRPC share the send path; a new protocol = a new sender, Engine untouched.
#pragma once

#include <atomic>
#include <memory>

#include "core/i_ui_delegate.hpp"
#include "core/types.hpp"

namespace core {

// Cancel token shared between Engine and sender. Sender polls isCancelled() in the send loop.
class CancelToken {
public:
    void cancel() { flag_.store(true, std::memory_order_relaxed); }
    bool isCancelled() const { return flag_.load(std::memory_order_relaxed); }
private:
    std::atomic<bool> flag_{false};
};

class IRequestSender {
public:
    virtual ~IRequestSender() = default;

    // Send a RESOLVED request. Called from a background thread (Engine already dispatched to a pool).
    // Sender is responsible for calling exactly ONE terminal callback on the delegate.
    virtual void send(const ResolvedRequest& req,
                      RequestHandle handle,
                      IUiDelegate& delegate,
                      const std::shared_ptr<CancelToken>& cancel) = 0;
};

} // namespace core

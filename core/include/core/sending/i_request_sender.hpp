// core/i_request_sender.hpp — Trừu tượng hoá giao thức (README §8.1).
// HTTP và gRPC dùng chung đường gửi; thêm protocol mới = sender mới, Engine không đụng.
#pragma once

#include <atomic>
#include <memory>

#include "core/i_ui_delegate.hpp"
#include "core/types.hpp"

namespace core {

// Token huỷ chia sẻ giữa Engine và sender. Sender poll isCancelled() trong vòng gửi.
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

    // Gửi request ĐÃ resolve. Gọi từ thread nền (Engine đã đẩy ra pool).
    // Sender chịu trách nhiệm gọi đúng MỘT callback terminal trên delegate.
    virtual void send(const ResolvedRequest& req,
                      RequestHandle handle,
                      IUiDelegate& delegate,
                      const std::shared_ptr<CancelToken>& cancel) = 0;
};

} // namespace core

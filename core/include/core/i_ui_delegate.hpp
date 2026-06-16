// core/i_ui_delegate.hpp — Port RA (Core → UI). README §2 / UI spec §2.2, §3.
// Core gọi NGƯỢC lại từ THREAD NỀN; mỗi adapter tự marshal về UI thread.
// Hợp đồng terminal: với mỗi handle tối đa MỘT trong {onResponse, onError}.
#pragma once

#include "core/types.hpp"

namespace core {

class IUiDelegate {
public:
    virtual ~IUiDelegate() = default;

    // Optional cho POC — tiến triển upload/download.
    virtual void onProgress(RequestHandle, const Progress&) {}

    // Terminal: hoàn tất thành công.
    virtual void onResponse(RequestHandle, const ApiResponse&) = 0;

    // Terminal: mạng/timeout/cancel/parse fail. kind == Cancelled sau khi cancel().
    virtual void onError(RequestHandle, const ApiError&) = 0;
};

} // namespace core

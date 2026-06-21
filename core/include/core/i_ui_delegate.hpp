// core/i_ui_delegate.hpp — OUTBOUND port (Core → UI). README §2 / UI spec §2.2, §3.
// Core calls BACK from a BACKGROUND THREAD; each adapter marshals to the UI thread itself.
// Terminal contract: at most ONE of {onResponse, onError} per handle.
#pragma once

#include "core/types.hpp"

namespace core {

class IUiDelegate {
public:
    virtual ~IUiDelegate() = default;

    // Optional for POC — upload/download progress.
    virtual void onProgress(RequestHandle, const Progress&) {}

    // Terminal: completed successfully.
    virtual void onResponse(RequestHandle, const ApiResponse&) = 0;

    // Terminal: network/timeout/cancel/parse fail. kind == Cancelled after cancel().
    virtual void onError(RequestHandle, const ApiError&) = 0;
};

} // namespace core

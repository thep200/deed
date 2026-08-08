#include "infra/transport/ws/ws_internal.hpp"

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <curl/curl.h>
#include <curl/websockets.h>

#include <nlohmann/json.hpp>

namespace core {
using namespace ws_detail;

namespace ws_detail {

// false if the send queue is full (backpressure) or the session is gone.
bool enqueue(const std::shared_ptr<WsSession>& s, OutFrame f) {
    std::lock_guard<std::mutex> lk(s->mu);
    if (s->done.load() || s->wantClose) return false;
    if (s->out.size() >= s->cfg.sendQueueMaxFrames ||
        s->outBytes + f.data.size() > s->cfg.sendQueueMaxBytes)
        return false;
    s->outBytes += f.data.size();
    s->out.push_back(std::move(f));
    return true;
}

// Per-frame log element: {"dir","type","ts","data"|"data_base64"}.
std::string frameEnvelope(StreamDirection dir, bool binary, long long offMs,
                          const std::uint8_t* data, std::size_t n) {
    nlohmann::json env;
    env["dir"] = (dir == StreamDirection::Outbound) ? "out" : "in";
    env["type"] = binary ? "binary" : "text";
    env["ts"] = offMs;
    if (binary) {
        env["data_base64"] = base64(data, n);
    } else {
        auto j = nlohmann::json::parse(data, data + n, nullptr, false);
        if (!j.is_discarded()) env["data"] = std::move(j);   // embed parsed JSON when possible
        else env["data"] = std::string(reinterpret_cast<const char*>(data), n);   // else raw string
    }
    return env.dump();
}

} // namespace ws_detail

namespace {

class WsChannel : public IStreamChannel {
public:
    explicit WsChannel(std::shared_ptr<WsSession> s) : s_(std::move(s)) {}
    bool sendText(const std::string& utf8) override {
        return enqueue(s_, OutFrame{std::vector<std::uint8_t>(utf8.begin(), utf8.end()),
                                    CURLWS_TEXT, 0});
    }
    bool sendBinary(const std::vector<std::uint8_t>& bytes) override {
        return enqueue(s_, OutFrame{bytes, CURLWS_BINARY, 0});
    }
    void close(int code, const std::string& reason) override { wsRequestClose(s_, code, reason); }
    bool isOpen() const override { return s_->open.load() && !s_->done.load(); }

private:
    std::shared_ptr<WsSession> s_;
};

} // namespace

std::shared_ptr<WsSession> wsMakeSession(const WsConfig& cfg) {
    auto s = std::make_shared<WsSession>();
    s->cfg = cfg;
    return s;
}

std::shared_ptr<IStreamChannel> wsMakeChannel(const std::shared_ptr<WsSession>& session) {
    return std::make_shared<WsChannel>(session);
}

void wsRequestClose(const std::shared_ptr<WsSession>& session, int code, const std::string& reason) {
    if (!session) return;
    {
        std::lock_guard<std::mutex> lk(session->mu);
        session->wantClose = true;
        session->closeCode = code;
        session->closeReason = reason;
    }
    // Not open yet == still in the handshake, where the pump's checks don't run. Pull the socket down.
    // Once open, leave it alone: the pump owes the peer a graceful CLOSE frame.
    if (!session->open.load()) session->sockets->abort();
}

} // namespace core

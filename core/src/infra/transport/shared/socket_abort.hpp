// Aborts a libcurl transfer that is still CONNECTING: libcurl runs no callback until bytes flow, so a
// progress-callback abort can't touch a connect parked on a dead SYN. Owning socket open/close lets
// Cancel shutdown() the fd — the blocked connect returns immediately.
#pragma once

#include <algorithm>
#include <mutex>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

#include <curl/curl.h>

namespace core {

class SocketAbort {
public:
    // CURLOPT_OPENSOCKETFUNCTION
    static curl_socket_t openCb(void* p, curlsocktype, struct curl_sockaddr* a) {
        auto* self = static_cast<SocketAbort*>(p);
        curl_socket_t fd = ::socket(a->family, a->socktype, a->protocol);
        if (fd == CURL_SOCKET_BAD) return CURL_SOCKET_BAD;
        std::lock_guard<std::mutex> lk(self->mu_);
        if (self->aborted_) { ::close(fd); return CURL_SOCKET_BAD; }   // cancel landed between two sockets
        self->fds_.push_back(fd);
        return fd;
    }

    // CURLOPT_CLOSESOCKETFUNCTION
    static int closeCb(void* p, curl_socket_t fd) {
        auto* self = static_cast<SocketAbort*>(p);
        {
            std::lock_guard<std::mutex> lk(self->mu_);
            self->fds_.erase(std::remove(self->fds_.begin(), self->fds_.end(), fd), self->fds_.end());
        }
        ::close(fd);
        return 0;
    }

    // shutdown(), never close(): curl still owns the fd and frees it through closeCb, so no window exists
    // where another thread could be handed the same fd number.
    void abort() {
        std::lock_guard<std::mutex> lk(mu_);
        aborted_ = true;
        for (curl_socket_t fd : fds_) ::shutdown(fd, SHUT_RDWR);
    }

    // Wire both hooks onto an easy handle. `self` must outlive the transfer.
    static void install(CURL* handle, SocketAbort* self) {
        if (!handle || !self) return;
        curl_easy_setopt(handle, CURLOPT_OPENSOCKETFUNCTION, &SocketAbort::openCb);
        curl_easy_setopt(handle, CURLOPT_OPENSOCKETDATA, self);
        curl_easy_setopt(handle, CURLOPT_CLOSESOCKETFUNCTION, &SocketAbort::closeCb);
        curl_easy_setopt(handle, CURLOPT_CLOSESOCKETDATA, self);
    }

private:
    std::mutex mu_;
    std::vector<curl_socket_t> fds_;   // sockets curl holds right now (redirect/reconnect -> more than one)
    bool aborted_ = false;
};

} // namespace core

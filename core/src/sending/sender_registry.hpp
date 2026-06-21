// sender_registry.hpp — map RequestType -> IRequestSender (README §8.1).
// Engine looks at request.type -> queries the registry -> sender->send(...). Adding a protocol does not touch Engine.
#pragma once

#include <map>
#include <memory>

#include "core/sending/i_request_sender.hpp"
#include "core/types.hpp"

namespace core {

class SenderRegistry {
public:
    void registerSender(RequestType type, std::unique_ptr<IRequestSender> sender) {
        senders_[type] = std::move(sender);
    }
    IRequestSender* get(RequestType type) const {
        auto it = senders_.find(type);
        return it == senders_.end() ? nullptr : it->second.get();
    }
private:
    std::map<RequestType, std::unique_ptr<IRequestSender>> senders_;
};

} // namespace core

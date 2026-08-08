#pragma once

#include "core/domain/request/request_traits.hpp"

namespace core::domain {

inline RequestModel::Payload defaultPayloadFor(RequestType type) {
  return dispatchType(type, [](auto tag) {
    return RequestTraits<typename decltype(tag)::type>::makeDefault();
  });
}

} // namespace core::domain

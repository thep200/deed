#pragma once

#include "infra/serialization/json_codec.hpp"
#include "core/domain/ports/driven/i_json_validator.hpp"

namespace core::infra {

class JsonValidator final : public domain::IJsonValidator {
public:
  domain::Status validate(const domain::JsonText &text) const override {
    if (text.empty()) return domain::ok(); // empty body is valid (no payload)
    try {
      (void)core::codec::parseGuarded(text.text());
      return domain::ok();
    } catch (const std::exception &e) {
      return domain::Status::fail({domain::ErrorCode::Parse, e.what(), "body.json"});
    }
  }
};

} // namespace core::infra

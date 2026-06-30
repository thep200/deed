// core/src/infra/platform/json_validator.hpp — IJsonValidator impl (REFACTOR_SPEC §8). Uses the depth-guarded
// parser; this is INFRA, so touching nlohmann here is allowed (the golden rule only forbids it in domain/app).
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

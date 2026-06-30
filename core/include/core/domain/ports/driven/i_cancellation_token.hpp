// core/domain/ports/driven/i_cancellation_token.hpp — cooperative cancel signal (REFACTOR_SPEC §6.3/§7.4).
#pragma once

namespace core::domain {

class ICancellationToken {
public:
  virtual ~ICancellationToken() = default;
  virtual bool cancelled() const noexcept = 0;
};

} // namespace core::domain

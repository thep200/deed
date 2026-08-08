// Stateless: every per-call resource (fd, LDAP handle, cancel bridge) lives on the execute() stack,
// so concurrent sends never share cancel state.
#pragma once

#include "infra/transport/typed_sender.hpp"

namespace core::infra {

class NativeLdapSender final : public TypedSender<domain::LdapRequest> {
protected:
  domain::Status executeTyped(const domain::RequestModel &resolved,
                              const domain::LdapRequest &ldap, domain::IResponseSink &sink,
                              const domain::ICancellationToken &cancel) override;
};

} // namespace core::infra

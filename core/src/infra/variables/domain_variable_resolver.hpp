// core/src/infra/variables/domain_variable_resolver.hpp — IVariableResolver over the proven
// core::VariableResolver (REFACTOR_SPEC §6.3, closes the new stack's variable-resolution gap).
// Resolves {{vars}} by bridging domain -> legacy struct, substituting each string field with the existing
// pure resolver, then bridging back. Reuses battle-tested logic instead of re-implementing it.
#pragma once

#include "core/domain/ports/i_variable_resolver.hpp"

namespace core::infra {

class DomainVariableResolver final : public domain::IVariableResolver {
public:
  domain::Result<domain::RequestModel> resolve(const domain::RequestModel &model,
                                               const domain::VariableScope &scope) const override;
};

} // namespace core::infra

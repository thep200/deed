#pragma once

#include "core/domain/ports/driven/i_variable_resolver.hpp"

namespace core::infra {

class DomainVariableResolver final : public domain::IVariableResolver {
public:
  domain::Result<domain::RequestModel> resolve(const domain::RequestModel &model,
                                               const domain::VariableScope &scope) const override;
};

} // namespace core::infra

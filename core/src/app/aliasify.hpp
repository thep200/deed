#pragma once

#include <string>
#include <utility>
#include <vector>

#include "core/domain/request/request_model.hpp"

namespace core::app {

// literal -> {{alias}} rewrite: prefix match for url/target/brokers, whole-value for kv/auth; never touches body/query/message.
core::domain::RequestModel
aliasifyModel(const core::domain::RequestModel &model,
              const std::vector<std::pair<std::string, std::string>> &vars);

} // namespace core::app

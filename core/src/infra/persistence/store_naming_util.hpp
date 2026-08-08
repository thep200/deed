#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "core/domain/request/request_model.hpp"
#include "core/domain/request/request_type.hpp"

namespace core::store_detail {

bool isReservedDir(const std::string &name);
bool isConfigFile(const std::string &name);
bool isHidden(const std::string &name);
std::string genId();
core::domain::RequestModel withIdName(const core::domain::RequestModel &m, std::string id, std::string name);

std::string orderOf(const std::string &filename);
std::vector<std::string> sortedOrderKeys(const std::filesystem::path &dir);
std::string keyForEnd(const std::filesystem::path &dir);
bool clashesIgnoringCase(const std::filesystem::path &dir, const std::string &name, const std::string &self);
std::string pickKeyBetween(const std::filesystem::path &dir, const std::string &prev, const std::string &next,
                           const std::string &rest, const std::string &self);
std::string uniquePath(const std::filesystem::path &dir, const std::string &slug, const std::string &ext);
std::string uniqueEncodedName(const std::filesystem::path &dir, const std::string &id,
                              RequestType type, const std::string &method,
                              const std::string &displayName,
                              const std::string &order = "");

} // namespace core::store_detail

#include "app/repo_adapters.hpp"

#include <chrono>

namespace core::app {

std::int64_t nowEpochMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

} // namespace core::app

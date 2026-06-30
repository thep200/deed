// repository_test.cpp — REFACTOR_SPEC §8.3: ICollectionRepository returns DOMAIN objects.
// Creates a request via the legacy store, loads it through the repository (domain RequestModel), mutates +
// saves through the repository, reloads, and lists the level — all in domain terms, never touching JSON.
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

#include "core/domain/request/request_model.hpp"
#include "infra/persistence/collection_repository.hpp"
#include "core/persistence/stores.hpp"

namespace fs = std::filesystem;
using namespace core::domain;

static int rp_pass = 0, rp_fail = 0;
#define RP_CHECK(cond, msg)                                                                        \
  do {                                                                                             \
    if (cond) { ++rp_pass; }                                                                       \
    else { ++rp_fail; std::printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); }             \
  } while (0)

int run_repository_tests() {
  std::printf("[collection_repository]\n");
  auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  auto base = fs::temp_directory_path() / ("deed_repo_test_" + std::to_string(stamp));
  fs::remove_all(base);
  fs::create_directories(base);

  auto store = std::make_shared<core::CollectionStore>(base.string());
  std::string rel = store->createRequest("", core::RequestType::Http, "Get Thing");

  core::infra::CollectionRepository repo(store);

  // load -> domain model
  auto loaded = repo.load(rel);
  RP_CHECK(loaded.isOk(), "repo load returns domain RequestModel");
  if (loaded.isOk()) {
    RP_CHECK(loaded.value().type() == RequestType::Http, "loaded type is http");
    RP_CHECK(loaded.value().name() == "Get Thing", "loaded name preserved");

    // mutate (immutable update) + save through the repo
    RequestModel updated = loaded.value().withName("Renamed Thing");
    auto saved = repo.save(rel, updated);
    RP_CHECK(saved.isOk(), "repo save returns relPath");
    if (saved.isOk()) {
      auto reloaded = repo.load(saved.value());
      RP_CHECK(reloaded.isOk() && reloaded.value().name() == "Renamed Thing",
               "repo save->load round-trips the change");
    }
  }

  // listLevel -> domain CollectionNodes
  auto nodes = repo.listLevel("");
  bool found = false;
  for (const auto &n : nodes)
    if (!n.isFolder && n.type && *n.type == RequestType::Http) found = true;
  RP_CHECK(found, "listLevel returns the http request node");

  // missing path -> NotFound (no throw across the boundary)
  RP_CHECK(!repo.load("does/not/exist.json").isOk(), "missing path -> Result error, no throw");

  fs::remove_all(base);
  std::printf("  repository: %d passed, %d failed\n", rp_pass, rp_fail);
  return rp_fail;
}

// Round-trips go through the repository PORT interfaces, not the stores directly.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>

#include "core/app/persistence_repositories.hpp"

namespace fs = std::filesystem;
using namespace core::app;

static int pr_pass = 0, pr_fail = 0;
#define PR_CHECK(cond, msg)                                                                        \
  do {                                                                                             \
    if (cond) { ++pr_pass; }                                                                       \
    else { ++pr_fail; std::printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); }             \
  } while (0)

int run_persistence_repo_tests() {
  std::printf("[persistence_repos]\n");
  auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
  auto root = fs::temp_directory_path() / ("deed_prepo_" + std::to_string(stamp));
  fs::create_directories(root);

  // Environments
  core::EnvironmentStore envStore(root.string());
  EnvironmentRepository envs(envStore);
  core::Environment e;
  e.name = "staging";
  e.keys.push_back({"baseUrl", "https://staging.example.com", true, false});
  envs.save(e);
  auto names = envs.list();
  PR_CHECK(std::find(names.begin(), names.end(), "staging") != names.end(), "env repo list/save");
  auto loaded = envs.load("staging");
  PR_CHECK(loaded.keys.size() == 1 && loaded.keys[0].key == "baseUrl", "env repo load round-trip");
  envs.remove("staging");
  PR_CHECK(envs.list().empty(), "env repo remove");

  // reserved Global: save/load OK, list() hides.
  core::Environment g;
  g.name = core::kGlobalEnvName;
  g.keys.push_back({"k", "v", true, false});
  envs.save(g);
  PR_CHECK(envs.list().empty(), "env repo list hides reserved Global");
  PR_CHECK(envs.load(core::kGlobalEnvName).keys.size() == 1, "env repo load(Global) works");

  // Session
  core::SessionStore sessStore(root.string());
  SessionRepository sess(sessStore);
  sess.saveLastOpened("folder/req.json");
  sess.setActiveEnv("prod");
  PR_CHECK(sess.loadLastOpened() == "folder/req.json", "session repo lastOpened round-trip");
  PR_CHECK(sess.getActiveEnv() == "prod", "session repo activeEnv round-trip");
  sess.setActiveEnv(core::kGlobalEnvName);
  PR_CHECK(sess.getActiveEnv() == "prod", "setActiveEnv(Global) ignored (reserved)");

  // App config
  core::AppConfigStore cfgStore((root / "config.json").string());
  AppConfigRepository cfg(cfgStore);
  core::AppConfig ac;
  ac.fontName = "Monaco";
  ac.fontSize = 13;
  cfg.save(ac);
  auto rc = cfg.load();
  PR_CHECK(rc.fontName == "Monaco" && rc.fontSize == 13, "appconfig repo round-trip");

  // Collection store (the legacy store surface; the domain ICollectionRepository adapter lives in composition_root).
  auto collRoot = root / "collection";
  fs::create_directories(collRoot);
  core::CollectionStore collStore(collRoot.string());
  std::string rel = collStore.createRequest("", core::RequestType::Http, "Ping");
  PR_CHECK(!rel.empty(), "collection store createRequest");
  auto level = collStore.scanLevel("");
  PR_CHECK(!level.empty(), "collection store scanLevel sees the new request");
  auto cm = collStore.loadRequest(rel);
  PR_CHECK(cm.type() == core::domain::RequestType::Http, "collection store loadRequest type");
  PR_CHECK(collStore.findRelPathById(cm.id().get()) == rel, "collection store findRelPathById");
  std::string folder = collStore.createFolder("", "Group");
  PR_CHECK(!folder.empty(), "collection store createFolder");
  collStore.remove(rel);
  PR_CHECK(collStore.findRelPathById(cm.id().get()).empty(), "collection store remove");

  fs::remove_all(root);
  std::printf("  persistence_repos: %d passed, %d failed\n", pr_pass, pr_fail);
  return pr_fail;
}

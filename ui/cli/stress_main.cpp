// ui/cli/stress_main.cpp — target `deed_stress` (STRESS_TEST.md §4).
// Deterministic (seeded) loop mixing Core operations: collection CRUD, load/release,
// import cURL/grpcurl (including garbage), resolve {{var}}, ResponseCache put/get/remove with cap
// checks, ThreadPool flood. Each iteration logs RAM (phys_footprint) + cache size to CSV.
//
// Run under ASan / TSan / `leaks` (see scripts/stress.sh) to catch UAF / race / RAM leak.
// Built ONLY when -DDEED_BUILD_STRESS=ON (not shipped in release).
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "core/cache.hpp"
#include "core/import_export/importer.hpp"
#include "core/persistence/stores.hpp" // CollectionStore (CRUD) — stress drives the store directly, no Engine
#include "core/infra/mem_probe.hpp"
#include "core/variables/variable_resolver.hpp"

#include "infra/thread_pool.hpp"   // core/src internal (header-only) — via the target's include dir

using namespace core;
namespace fs = std::filesystem;

namespace {

struct Args {
    long long iters = 50000;
    unsigned seed = 42;
    std::string log;       // CSV path (empty -> no logging)
    long long idleEvery = 500;  // K: every K iterations -> idle checkpoint
};

Args parseArgs(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string k = argv[i];
        auto next = [&](const char* def) -> std::string {
            return (i + 1 < argc) ? std::string(argv[++i]) : std::string(def);
        };
        if (k == "--iters") a.iters = std::stoll(next("50000"));
        else if (k == "--seed") a.seed = (unsigned)std::stoul(next("42"));
        else if (k == "--log") a.log = next("");
        else if (k == "--idle-every") a.idleEvery = std::stoll(next("500"));
    }
    return a;
}

// Import corpus — includes both VALID inputs and GARBAGE (parser must not crash/leak on garbage).
const std::vector<std::string>& curlCorpus() {
    static const std::vector<std::string> c = {
        "curl https://example.com/api",
        "curl -X POST https://api.test/v1/users -H 'Content-Type: application/json' -d '{\"a\":1}'",
        "curl -u user:pass -H 'X-Token: abc' https://h/g?x=1&y=2",
        "curl --data-binary @/tmp/x.bin -X PUT http://localhost:8000/up",
        "curl",                                  // garbage: missing url
        "curl -X",                               // garbage: dangling flag
        "not a curl command at all {{{",         // pure garbage
        "curl -H -H -H ---- '''' \"\"\"",        // garbage: malformed flags
    };
    return c;
}
const std::vector<std::string>& grpcCorpus() {
    static const std::vector<std::string> c = {
        "grpcurl -plaintext localhost:50051 pkg.Service/Method",
        "grpcurl -d '{\"x\":1}' host:443 a.b.C/Do",
        "grpc://localhost:50051/pkg.Service/Method",
        "grpcs://h:443/a.B/C",
        "grpcurl",                               // garbage
        "grpcurl -d",                            // garbage: dangling
        "::::///garbage",                        // garbage
    };
    return c;
}

// Template resolve — existing var, empty var, NON-existent var (kept literal).
const std::vector<std::string>& tplCorpus() {
    static const std::vector<std::string> c = {
        "{{baseUrl}}/users/{{id}}",
        "no vars here",
        "{{missing}} and {{baseUrl}}",
        "{{empty}}{{baseUrl}}{{missing}}",
        "{{{{nested}}}} {{ spaced }}",           // edge: nested braces / with spaces
    };
    return c;
}

std::string randBody(std::mt19937& rng, std::size_t n) {
    std::string s;
    s.resize(n);
    for (std::size_t i = 0; i < n; ++i) s[i] = char('a' + (rng() % 26));
    return s;
}

ResponseRecord makeRecord(std::mt19937& rng, std::size_t bodyBytes) {
    ResponseRecord r;
    r.isError = false;
    r.response.statusCode = 200;
    r.response.body = randBody(rng, bodyBytes);
    r.receivedAt = 0;
    return r;
}

} // namespace

int main(int argc, char** argv) {
    Args args = parseArgs(argc, argv);

    // Dedicated temp collection for this run (cleaned up at the end).
    fs::path root = fs::temp_directory_path() / ("deed_stress_" + std::to_string(args.seed));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    std::unique_ptr<memprobe::StructuredLogger> logger;
    if (!args.log.empty()) {
        logger = std::make_unique<memprobe::StructuredLogger>(args.log);
        if (!logger->ok()) std::cerr << "WARN: could not open log " << args.log << "\n";
    }

    std::mt19937 rng(args.seed);
    int failures = 0;  // invariant (cap) violation -> exit code != 0

    // Small cache to ACTUALLY hit eviction: RAM 4MB, disk 16MB, threshold 256KB.
    CacheConfig cc;
    cc.ramEffBytes = 4ull * 1024 * 1024;
    cc.diskEffBytes = 16ull * 1024 * 1024;
    cc.thresholdBytes = 256ull * 1024;
    cc.enabled = true;
    cc.persist = true;
    fs::path sessionDir = root / ".session";
    fs::create_directories(sessionDir, ec);
    auto cache = ResponseCache::create(cc, sessionDir.string());

    // ThreadPool reused across iterations (avoid creating/destroying the pool 50k times).
    ThreadPool pool;
    std::atomic<std::uint64_t> poolCounter{0};

    CollectionStore coll(root.string());
    CurlImporter curlImp;
    GrpcImporter grpcImp;

    std::vector<std::string> rels;   // requests currently on disk
    std::vector<std::string> ids;    // parallel ids used as cache keys
    std::string openId;              // the "open" request (single-active)

    auto logRow = [&](long long iter, const char* op, bool idle) {
        if (!logger) return;
        memprobe::StructuredLogger::Row r;
        r.iter = iter; r.op = op; r.idle = idle;
        r.ramCacheBytes = cache ? cache->l1UsedBytes() : 0;
        r.diskCacheBytes = cache ? cache->l2UsedBytes() : 0;
        r.openRequestId = idle ? "" : openId;
        logger->log(r);
    };

    auto checkCaps = [&](long long iter) {
        if (!cache) return;
        if (cache->l1UsedBytes() > cc.ramEffBytes) {
            std::cerr << "FAIL iter " << iter << ": RAM cache " << cache->l1UsedBytes()
                      << " > cap " << cc.ramEffBytes << "\n";
            ++failures;
        }
        if (cache->l2UsedBytes() > cc.diskEffBytes) {
            std::cerr << "FAIL iter " << iter << ": disk cache " << cache->l2UsedBytes()
                      << " > cap " << cc.diskEffBytes << "\n";
            ++failures;
        }
    };

    for (long long iter = 0; iter < args.iters; ++iter) {
        int op = rng() % 8;
        const char* opName = "noop";
        try {
            switch (op) {
                case 0: {  // create request
                    opName = "create";
                    std::string name = "req_" + std::to_string(iter);
                    RequestType t = (rng() % 2) ? RequestType::Grpc : RequestType::Http;
                    std::string rel = coll.createRequest("", t, name);
                    rels.push_back(rel);
                    auto m = coll.loadRequest(rel);
                    ids.push_back(m.id().get());
                    break;
                }
                case 1: {  // rename request
                    opName = "rename";
                    if (rels.empty()) break;
                    std::size_t i = rng() % rels.size();
                    std::string nn = "ren_" + std::to_string(iter);
                    rels[i] = coll.rename(rels[i], nn);
                    break;
                }
                case 2: {  // delete request (free it)
                    opName = "delete";
                    if (rels.empty()) break;
                    std::size_t i = rng() % rels.size();
                    coll.remove(rels[i]);
                    if (openId == ids[i]) openId.clear();
                    if (cache) cache->remove(ids[i]);
                    rels.erase(rels.begin() + (long)i);
                    ids.erase(ids.begin() + (long)i);
                    break;
                }
                case 3: {  // load + release (check single-active: only 1 live model)
                    opName = "load";
                    if (rels.empty()) break;
                    std::size_t i = rng() % rels.size();
                    auto m = coll.loadRequest(rels[i]);
                    openId = m.id().get();   // old model destroyed on scope exit -> single-active
                    break;
                }
                case 4: {  // import cURL / grpcurl (including garbage)
                    opName = "import";
                    const auto& cs = curlCorpus();
                    const auto& gs = grpcCorpus();
                    (void)curlImp.parse(cs[rng() % cs.size()]);
                    (void)grpcImp.parse(gs[rng() % gs.size()]);
                    break;
                }
                case 5: {  // resolve {{var}}
                    opName = "resolve";
                    std::map<std::string, std::string> vars = {
                        {"baseUrl", "https://h"}, {"id", "42"}, {"empty", ""}};
                    const auto& ts = tplCorpus();
                    (void)VariableResolver::resolve(ts[rng() % ts.size()], vars);
                    break;
                }
                case 6: {  // ResponseCache put/get/remove (small < threshold & large > threshold)
                    opName = "cache";
                    if (!cache) break;
                    if (ids.empty()) break;
                    std::size_t i = rng() % ids.size();
                    bool large = (rng() % 4 == 0);   // ~25% large responses (exceed threshold)
                    std::size_t sz = large ? (512ull * 1024 + rng() % (1024 * 1024))   // 0.5–1.5MB
                                           : (rng() % (200 * 1024));                    // < 200KB
                    cache->put(ids[i], makeRecord(rng, sz));
                    (void)cache->get(ids[i]);
                    if (rng() % 5 == 0) cache->remove(ids[i]);
                    checkCaps(iter);
                    break;
                }
                case 7: {  // ThreadPool flood — many short tasks (check no deadlock/no leak)
                    opName = "threadpool";
                    int batch = 200 + (int)(rng() % 800);
                    for (int k = 0; k < batch; ++k)
                        pool.submit([&poolCounter] { poolCounter.fetch_add(1, std::memory_order_relaxed); });
                    break;
                }
            }
        } catch (const std::exception& e) {
            // Data-driven errors (e.g. duplicate rename) are acceptable — MUST NOT crash/leak.
            // Sanitizer/leaks will catch the real problems; here we just acknowledge it.
        }

        logRow(iter, opName, false);

        // Idle checkpoint: "close" the open request -> baseline state for RAM-leak analysis.
        if (args.idleEvery > 0 && (iter % args.idleEvery) == (args.idleEvery - 1)) {
            openId.clear();
            logRow(iter, "idle", true);
        }
    }

    if (logger) logger->flush();
    fs::remove_all(root, ec);

    std::cout << "deed_stress done: iters=" << args.iters << " seed=" << args.seed
              << " pool_tasks=" << poolCounter.load() << " failures=" << failures
              << " final_footprint_mb="
              << (double)memprobe::PhysFootprintBytes() / (1024.0 * 1024.0) << "\n";
    return failures == 0 ? 0 : 1;
}

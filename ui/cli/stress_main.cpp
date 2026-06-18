// ui/cli/stress_main.cpp — target `deed_stress` (STRESS_TEST.md §4).
// Vòng lặp tất định (có seed) trộn các thao tác Core: collection CRUD, load/release,
// import cURL/grpcurl (kể cả rác), resolve {{var}}, ResponseCache put/get/remove với kiểm
// cap, ThreadPool flood. Mỗi vòng log RAM (phys_footprint) + dung lượng cache ra CSV.
//
// Chạy dưới ASan / TSan / `leaks` (xem scripts/stress.sh) để bắt UAF / race / rò RAM.
// CHỈ build khi -DDEED_BUILD_STRESS=ON (không ship release).
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
#include "core/engine.hpp"
#include "core/import_export/importer.hpp"
#include "core/infra/mem_probe.hpp"
#include "core/variables/variable_resolver.hpp"

#include "infra/thread_pool.hpp"   // core/src nội bộ (header-only) — qua include dir của target

using namespace core;
namespace fs = std::filesystem;

namespace {

struct Args {
    long long iters = 50000;
    unsigned seed = 42;
    std::string log;       // CSV path (rỗng -> không log)
    long long idleEvery = 500;  // K: cứ K vòng -> idle checkpoint
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

// Corpus import — gồm cả input HỢP LỆ lẫn RÁC (parser phải không crash/không rò trên rác).
const std::vector<std::string>& curlCorpus() {
    static const std::vector<std::string> c = {
        "curl https://example.com/api",
        "curl -X POST https://api.test/v1/users -H 'Content-Type: application/json' -d '{\"a\":1}'",
        "curl -u user:pass -H 'X-Token: abc' https://h/g?x=1&y=2",
        "curl --data-binary @/tmp/x.bin -X PUT http://localhost:8000/up",
        "curl",                                  // rác: thiếu url
        "curl -X",                               // rác: cờ treo
        "not a curl command at all {{{",         // rác hoàn toàn
        "curl -H -H -H ---- '''' \"\"\"",        // rác: cờ hỏng
    };
    return c;
}
const std::vector<std::string>& grpcCorpus() {
    static const std::vector<std::string> c = {
        "grpcurl -plaintext localhost:50051 pkg.Service/Method",
        "grpcurl -d '{\"x\":1}' host:443 a.b.C/Do",
        "grpc://localhost:50051/pkg.Service/Method",
        "grpcs://h:443/a.B/C",
        "grpcurl",                               // rác
        "grpcurl -d",                            // rác: treo
        "::::///garbage",                        // rác
    };
    return c;
}

// Template resolve — biến tồn tại, biến rỗng, biến KHÔNG tồn tại (giữ literal).
const std::vector<std::string>& tplCorpus() {
    static const std::vector<std::string> c = {
        "{{baseUrl}}/users/{{id}}",
        "no vars here",
        "{{missing}} and {{baseUrl}}",
        "{{empty}}{{baseUrl}}{{missing}}",
        "{{{{nested}}}} {{ spaced }}",           // biên: dấu ngoặc lồng / có space
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
    r.response.statusText = "OK";
    r.response.body = randBody(rng, bodyBytes);
    r.response.sizeBytes = (std::int64_t)bodyBytes;
    r.receivedAt = 0;
    return r;
}

} // namespace

int main(int argc, char** argv) {
    Args args = parseArgs(argc, argv);

    // Temp collection riêng cho lần chạy (dọn ở cuối).
    fs::path root = fs::temp_directory_path() / ("deed_stress_" + std::to_string(args.seed));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    std::unique_ptr<memprobe::StructuredLogger> logger;
    if (!args.log.empty()) {
        logger = std::make_unique<memprobe::StructuredLogger>(args.log);
        if (!logger->ok()) std::cerr << "WARN: không mở được log " << args.log << "\n";
    }

    std::mt19937 rng(args.seed);
    int failures = 0;  // vi phạm bất biến (cap) -> exit code != 0

    // Cache nhỏ để THỰC SỰ chạm eviction: RAM 4MB, disk 16MB, threshold 256KB.
    CacheConfig cc;
    cc.ramEffBytes = 4ull * 1024 * 1024;
    cc.diskEffBytes = 16ull * 1024 * 1024;
    cc.thresholdBytes = 256ull * 1024;
    cc.enabled = true;
    cc.persist = true;
    fs::path sessionDir = root / ".session";
    fs::create_directories(sessionDir, ec);
    auto cache = ResponseCache::create(cc, sessionDir.string());

    // ThreadPool dùng lại qua các vòng (tránh tạo/huỷ pool 50k lần).
    ThreadPool pool;
    std::atomic<std::uint64_t> poolCounter{0};

    Engine engine(EngineConfig{root.string(), ""});
    CurlImporter curlImp;
    GrpcImporter grpcImp;

    std::vector<std::string> rels;   // request đang tồn tại trên đĩa
    std::vector<std::string> ids;    // id song song để dùng làm khoá cache
    std::string openId;              // request "đang mở" (single-active)

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
                    std::string rel = engine.collection().createRequest("", t, name);
                    rels.push_back(rel);
                    RequestModel m = engine.collection().loadRequest(rel);
                    ids.push_back(m.id);
                    break;
                }
                case 1: {  // rename request
                    opName = "rename";
                    if (rels.empty()) break;
                    std::size_t i = rng() % rels.size();
                    std::string nn = "ren_" + std::to_string(iter);
                    rels[i] = engine.collection().rename(rels[i], nn);
                    break;
                }
                case 2: {  // delete request (giải phóng)
                    opName = "delete";
                    if (rels.empty()) break;
                    std::size_t i = rng() % rels.size();
                    engine.collection().remove(rels[i]);
                    if (openId == ids[i]) openId.clear();
                    if (cache) cache->remove(ids[i]);
                    rels.erase(rels.begin() + (long)i);
                    ids.erase(ids.begin() + (long)i);
                    break;
                }
                case 3: {  // load + release (kiểm single-active: chỉ 1 model sống)
                    opName = "load";
                    if (rels.empty()) break;
                    std::size_t i = rng() % rels.size();
                    RequestModel m = engine.collection().loadRequest(rels[i]);
                    openId = m.id;           // model cũ bị huỷ ngay khi ra scope -> single-active
                    break;
                }
                case 4: {  // import cURL / grpcurl (kể cả rác)
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
                case 6: {  // ResponseCache put/get/remove (nhỏ < threshold & lớn > threshold)
                    opName = "cache";
                    if (!cache) break;
                    if (ids.empty()) break;
                    std::size_t i = rng() % ids.size();
                    bool large = (rng() % 4 == 0);   // ~25% response lớn (vượt threshold)
                    std::size_t sz = large ? (512ull * 1024 + rng() % (1024 * 1024))   // 0.5–1.5MB
                                           : (rng() % (200 * 1024));                    // < 200KB
                    cache->put(ids[i], makeRecord(rng, sz));
                    (void)cache->get(ids[i]);
                    if (rng() % 5 == 0) cache->remove(ids[i]);
                    checkCaps(iter);
                    break;
                }
                case 7: {  // ThreadPool flood — nhiều task ngắn (kiểm không deadlock/không rò)
                    opName = "threadpool";
                    int batch = 200 + (int)(rng() % 800);
                    for (int k = 0; k < batch; ++k)
                        pool.submit([&poolCounter] { poolCounter.fetch_add(1, std::memory_order_relaxed); });
                    break;
                }
            }
        } catch (const std::exception& e) {
            // Lỗi do dữ liệu (vd rename trùng) là chấp nhận được — KHÔNG được crash/rò.
            // Sanitizer/leaks sẽ bắt vấn đề thật; ở đây chỉ ghi nhận.
        }

        logRow(iter, opName, false);

        // Idle checkpoint: "đóng" request đang mở -> trạng thái baseline để phân tích rò RAM.
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

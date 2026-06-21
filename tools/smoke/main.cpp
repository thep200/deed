// Throwaway smoke test: proves all 3 libraries (cpr/libcurl, nlohmann-json, grpc++)
// configure + link on this machine. Delete this file when real code begins.
#include <cstdio>

#include <nlohmann/json.hpp>
#include <cpr/cpr.h>
#include <grpcpp/grpcpp.h>

int main() {
    nlohmann::json j = {{"toolchain", "ok"}};
    cpr::Session session;   // force-link cpr/libcurl (no network call)
    (void)session;

    std::printf("nlohmann/json : %s\n", j.dump().c_str());
    std::printf("cpr/libcurl   : linked\n");
    std::printf("gRPC          : %s\n", grpc::Version().c_str());
    std::puts("Setup OK - everything links.");
    return 0;
}

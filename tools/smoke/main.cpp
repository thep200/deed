// Throwaway smoke test: chung minh ca 3 thu vien (cpr/libcurl, nlohmann-json, grpc++)
// configure + link duoc tren may. Xoa file nay khi bat dau code that.
#include <cstdio>

#include <nlohmann/json.hpp>
#include <cpr/cpr.h>
#include <grpcpp/grpcpp.h>

int main() {
    nlohmann::json j = {{"toolchain", "ok"}};
    cpr::Session session;   // ep link cpr/libcurl (khong goi mang)
    (void)session;

    std::printf("nlohmann/json : %s\n", j.dump().c_str());
    std::printf("cpr/libcurl   : linked\n");
    std::printf("gRPC          : %s\n", grpc::Version().c_str());
    std::puts("Setup OK - moi thu link duoc.");
    return 0;
}

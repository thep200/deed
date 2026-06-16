add_executable(smoke main.cpp)
target_link_libraries(smoke PRIVATE
  cpr::cpr
  nlohmann_json::nlohmann_json
  gRPC::grpc++
)

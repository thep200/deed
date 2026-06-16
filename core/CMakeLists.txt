# core — static lib, pure C++ (cross-platform). UI link vào đây. README §3.
add_library(core STATIC
  src/types.cpp
  src/variable_resolver.cpp
  src/json_codec.cpp
  src/field_codec.cpp
  src/fs_util.cpp
  src/collection_store.cpp
  src/session_store.cpp
  src/secret_store.cpp
  src/environment_store.cpp
  src/app_config_store.cpp
  src/http_sender.cpp
  src/grpc_sender.cpp
  src/engine.cpp
  src/shell_tokenize.cpp
  src/curl_importer.cpp
  src/grpc_importer.cpp
)

# include/ = header public (UI chỉ thấy phần này); src/ = riêng tư.
target_include_directories(core
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(core
  PUBLIC
    nlohmann_json::nlohmann_json   # dùng trong codec (header nội bộ); DTO public không leak
  PRIVATE
    cpr::cpr                       # REST (libcurl)
    gRPC::grpc++                   # gRPC C++
    protobuf::libprotobuf          # descriptor / dynamic_message / json_util
    protobuf::libprotoc            # compiler::Importer / DiskSourceTree (nạp .proto runtime)
)

target_compile_features(core PUBLIC cxx_std_17)

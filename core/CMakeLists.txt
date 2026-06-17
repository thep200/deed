# core — static lib, pure C++ (cross-platform). UI link vào đây. README §3.
add_library(core STATIC
  src/domain/types.cpp
  src/variables/variable_resolver.cpp
  src/codec/json_codec.cpp
  src/codec/field_codec.cpp
  src/infra/fs_util.cpp
  src/persistence/collection_store.cpp
  src/persistence/session_store.cpp
  src/persistence/secret_store.cpp
  src/persistence/environment_store.cpp
  src/persistence/app_config_store.cpp
  src/sending/http_sender.cpp
  src/sending/grpc_sender.cpp
  src/app/engine.cpp
  src/import_export/shell_tokenize.cpp
  src/import_export/curl_importer.cpp
  src/import_export/curl_export.cpp
  src/import_export/grpc_importer.cpp
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

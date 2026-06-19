# --- Codegen stub client cho gRPC ServerReflection (third_party/grpc_reflection/reflection.proto) ---
# vcpkg KHÔNG export stub client reflection -> tự sinh bằng protoc + grpc_cpp_plugin.
set(REFL_PROTO_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../third_party/grpc_reflection)
set(REFL_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${REFL_GEN_DIR})
add_custom_command(
  OUTPUT ${REFL_GEN_DIR}/reflection.pb.cc ${REFL_GEN_DIR}/reflection.pb.h
         ${REFL_GEN_DIR}/reflection.grpc.pb.cc ${REFL_GEN_DIR}/reflection.grpc.pb.h
  COMMAND protobuf::protoc
  ARGS --proto_path=${REFL_PROTO_DIR}
       --cpp_out=${REFL_GEN_DIR}
       --grpc_out=${REFL_GEN_DIR}
       --plugin=protoc-gen-grpc=$<TARGET_FILE:gRPC::grpc_cpp_plugin>
       ${REFL_PROTO_DIR}/reflection.proto
  DEPENDS ${REFL_PROTO_DIR}/reflection.proto protobuf::protoc gRPC::grpc_cpp_plugin
  COMMENT "Generating gRPC reflection client stub"
  VERBATIM
)

# core — static lib, pure C++ (cross-platform). UI link vào đây. README §3.
add_library(core STATIC
  src/domain/types.cpp
  src/variables/variable_resolver.cpp
  src/codec/json_codec.cpp
  src/codec/field_codec.cpp
  src/infra/fs_util.cpp
  src/infra/mem_probe.cpp
  src/persistence/collection_store.cpp
  src/persistence/request_naming.cpp
  src/cache/ram_cache_driver.cpp
  src/cache/disk_cache_driver.cpp
  src/cache/response_cache.cpp
  src/persistence/session_store.cpp
  src/persistence/environment_store.cpp
  src/persistence/app_config_store.cpp
  src/sending/http_sender.cpp
  src/sending/grpc_sender.cpp
  src/sending/grpc_descriptors.cpp
  src/app/engine.cpp
  src/import_export/shell_tokenize.cpp
  src/import_export/curl_importer.cpp
  src/import_export/curl_export.cpp
  src/import_export/grpc_importer.cpp
  ${REFL_GEN_DIR}/reflection.pb.cc
  ${REFL_GEN_DIR}/reflection.grpc.pb.cc
)

# include/ = header public (UI chỉ thấy phần này); src/ = riêng tư.
# generated/ = stub reflection sinh lúc build (grpc_descriptors.cpp include "reflection.grpc.pb.h").
target_include_directories(core
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${REFL_GEN_DIR}
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

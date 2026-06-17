// core/field_codec.hpp — round-trip JSON-thô từng field cho editor (UI spec
// §3). UI hiển thị mỗi tab (Params/Headers/Body/Auth/Metadata/Message) là một ô
// JSON map thẳng vào field. Đây là port phụ để UI không phải tự parse JSON.
#pragma once

#include <string>
#include <vector>

#include "core/types.hpp"

namespace core::fieldcodec {

// KeyValue[] (headers/params/metadata) <-> JSON text.
std::string keyValuesToJson(const std::vector<KeyValue> &);
bool jsonToKeyValues(const std::string &, std::vector<KeyValue> &out,
                     std::string &err);

// Body{} <-> JSON text.
std::string bodyToJson(const Body &);
bool jsonToBody(const std::string &, Body &out, std::string &err);

// Auth{} <-> JSON text.
std::string authToJson(const Auth &);
bool jsonToAuth(const std::string &, Auth &out, std::string &err);

// Định dạng JSON: pretty (indent 2) hoặc compact. Không parse được -> trả
// nguyên văn.
std::string formatJson(const std::string &text, bool pretty);

// Encode: bọc text thành string literal JSON đã escape ("\"...\"").
std::string jsonEncodeString(const std::string &text);
// Decode: nếu text là string literal JSON -> trả nội dung bên trong; ngược lại
// giữ nguyên.
std::string jsonDecodeString(const std::string &text);

} // namespace core::fieldcodec

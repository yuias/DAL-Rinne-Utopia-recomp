#pragma once

// Small yyjson helpers shared by the VFS configuration parser and the dal_game config file reader.
// The header exposes yyjson types, so only targets that link yyjson include it.

#include <yyjson.h>

#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace dal::json {

using Value = const yyjson_val*;
using DocumentPtr = std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)>;

// Strict JSON with an optional UTF-8 BOM. Errors:
// "invalid JSON: <msg> at line L, column C (byte B)", then "duplicate key \"k\" in an object".
std::expected<DocumentPtr, std::string> readDocument(std::string_view text);

std::string_view stringOf(Value value);

// One of "null", "object", "array", "string", "boolean", "number".
const char* typeName(Value value);

// Calls fn(key, value) for each member in document order; stops when fn returns false.
template <typename Fn>
void forEachMember(Value object, Fn&& fn) {
    yyjson_obj_iter it = yyjson_obj_iter_with(object);
    while (yyjson_val* key = yyjson_obj_iter_next(&it)) {
        if (!fn(stringOf(key), static_cast<Value>(yyjson_obj_iter_get_val(key)))) {
            return;
        }
    }
}

// yyjson keeps duplicate object keys; returns a key that appears twice in one object, if any.
std::optional<std::string> findDuplicateKey(Value root);

}  // namespace dal::json

#include "core/json_read.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <vector>

namespace dal::json {

std::string_view stringOf(Value value) { return {yyjson_get_str(value), yyjson_get_len(value)}; }

const char* typeName(Value value) {
    if (yyjson_is_null(value)) return "null";
    if (yyjson_is_obj(value)) return "object";
    if (yyjson_is_arr(value)) return "array";
    if (yyjson_is_str(value)) return "string";
    if (yyjson_is_bool(value)) return "boolean";
    return "number";
}

// Walks every object in the document with an explicit stack, so nesting depth cannot overflow.
std::optional<std::string> findDuplicateKey(Value root) {
    std::vector<Value> pending{root};
    std::vector<std::string_view> keys;
    while (!pending.empty()) {
        const Value node = pending.back();
        pending.pop_back();
        const size_t first_child = pending.size();
        if (yyjson_is_obj(node)) {
            keys.clear();
            forEachMember(node, [&](std::string_view key, Value value) {
                keys.push_back(key);
                if (yyjson_is_obj(value) || yyjson_is_arr(value)) pending.push_back(value);
                return true;
            });
            std::ranges::sort(keys);
            const auto dup = std::ranges::adjacent_find(keys);
            if (dup != keys.end()) {
                return std::string(*dup);
            }
        } else if (yyjson_is_arr(node)) {
            yyjson_arr_iter it = yyjson_arr_iter_with(node);
            while (yyjson_val* value = yyjson_arr_iter_next(&it)) {
                if (yyjson_is_obj(value) || yyjson_is_arr(value)) pending.push_back(value);
            }
        }
        // The stack pops from the back: reverse the children so they are visited in document order.
        std::reverse(pending.begin() + static_cast<std::ptrdiff_t>(first_child), pending.end());
    }
    return std::nullopt;
}

std::expected<DocumentPtr, std::string> readDocument(std::string_view text) {
    // The input is not modified without YYJSON_READ_INSITU; the API just has no const overload.
    // A UTF-8 BOM is skipped, as Windows editors write one.
    yyjson_read_err read_err;
    DocumentPtr doc(
            yyjson_read_opts(const_cast<char*>(text.data()), text.size(), YYJSON_READ_ALLOW_BOM, nullptr, &read_err),
            &yyjson_doc_free);
    if (!doc) {
        const std::string_view before = text.substr(0, std::min(read_err.pos, text.size()));
        const auto line = 1 + std::ranges::count(before, '\n');
        const size_t column = before.size() - (before.rfind('\n') + 1);  // npos + 1 wraps to 0
        return std::unexpected(std::format("invalid JSON: {} at line {}, column {} (byte {})", read_err.msg, line,
                                           column + 1, read_err.pos));
    }
    if (const auto duplicate = findDuplicateKey(yyjson_doc_get_root(doc.get()))) {
        return std::unexpected(std::format("duplicate key \"{}\" in an object", *duplicate));
    }
    return doc;
}

}  // namespace dal::json

#include "app/config_file.h"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <format>
#include <optional>
#include <utility>

#include "core/json_read.h"

namespace dal {
namespace {

using json::Value;

std::unexpected<std::string> fail(std::string_view key, std::string_view what, Value value) {
    return std::unexpected(std::format("\"{}\": {}, got {}", key, what, json::typeName(value)));
}

std::expected<std::string, std::string> pathInput(std::string_view key, Value value,
                                                  const std::filesystem::path& base_dir) {
    if (!yyjson_is_str(value)) return fail(key, "expected a path string", value);
    const std::string_view text = json::stringOf(value);
    if (text.empty()) return std::unexpected(std::format("\"{}\": the path is empty", key));
    std::filesystem::path path;
    try {
        // Through char8_t, so the text is read as UTF-8 and not as the ANSI code page.
        path = std::filesystem::path(std::u8string(text.begin(), text.end()));
    } catch (const std::exception&) {
        return std::unexpected(std::format("\"{}\": the path is not valid UTF-8", key));
    }
    if (path.is_relative()) path = base_dir / path;
    const std::u8string utf8 = path.lexically_normal().u8string();
    return std::string(utf8.begin(), utf8.end());
}

// Unsigned integers are emitted as decimal text; the range checks stay with the option validation.
bool unsignedText(Value value, std::string& text) {
    if (yyjson_is_uint(value)) {
        text = std::to_string(yyjson_get_uint(value));
    } else if (yyjson_is_sint(value) && yyjson_get_sint(value) >= 0) {
        text = std::to_string(yyjson_get_sint(value));
    } else {
        return false;
    }
    return true;
}

std::expected<std::string, std::string> frameListInput(std::string_view key, Value value) {
    constexpr std::string_view kExpected = "expected a string or an array of positive integers";
    if (yyjson_is_str(value)) return std::string(json::stringOf(value));
    if (!yyjson_is_arr(value) || yyjson_arr_size(value) == 0) return fail(key, kExpected, value);
    std::string joined;
    yyjson_arr_iter it = yyjson_arr_iter_with(value);
    while (yyjson_val* item = yyjson_arr_iter_next(&it)) {
        std::string text;
        if (!unsignedText(item, text)) return fail(key, kExpected, item);
        if (!joined.empty()) joined += ',';
        joined += text;
    }
    return joined;
}

std::expected<std::string, std::string> inputFor(const OptionSpec& spec, std::string_view key, Value value,
                                                 const std::filesystem::path& base_dir) {
    if (yyjson_is_null(value)) {
        return std::unexpected(std::format("\"{}\": null is not a value; remove the key to use the default", key));
    }
    switch (spec.kind) {
        case OptionKind::Path: return pathInput(key, value, base_dir);
        case OptionKind::String:
            if (!yyjson_is_str(value)) return fail(key, "expected a string", value);
            return std::string(json::stringOf(value));
        case OptionKind::Unsigned: {
            std::string text;
            if (!unsignedText(value, text)) return fail(key, "expected a non-negative integer", value);
            return text;
        }
        case OptionKind::Flag:
            if (!yyjson_is_bool(value)) return fail(key, "expected true or false", value);
            return std::string(yyjson_get_bool(value) ? "true" : "false");
        case OptionKind::FrameList: return frameListInput(key, value);
    }
    std::unreachable();
}

}  // namespace

std::expected<ConfigFileContents, std::string> parseConfigFile(std::string_view text,
                                                               const std::filesystem::path& base_dir,
                                                               std::span<const OptionSpec> specs) {
    auto document = json::readDocument(text);
    if (!document) return std::unexpected(std::move(document.error()));
    const Value root = yyjson_doc_get_root(document->get());
    if (!yyjson_is_obj(root)) return std::unexpected("the top level must be an object");

    std::vector<std::string> keys;
    keys.reserve(specs.size());
    for (const OptionSpec& spec : specs) keys.push_back(configKeyFor(spec.name));

    ConfigFileContents contents;
    std::optional<std::string> error;
    json::forEachMember(root, [&](std::string_view key, Value value) {
        const auto found = std::ranges::find(keys, key);
        if (found == keys.end()) {
            contents.warnings.push_back(std::format("unknown key \"{}\" ignored", key));
            return true;
        }
        const OptionSpec& spec = specs[static_cast<std::size_t>(found - keys.begin())];
        auto input = inputFor(spec, key, value, base_dir);
        if (!input) {
            error = std::move(input.error());
            return false;
        }
        contents.values.push_back({std::string(spec.name), std::string(key), {std::move(*input)}});
        return true;
    });
    if (error) return std::unexpected(std::move(*error));
    return contents;
}

}  // namespace dal

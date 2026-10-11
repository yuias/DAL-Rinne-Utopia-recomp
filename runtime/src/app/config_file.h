// Reader of the dal_game config file (config.json): JSON text to option values. Knows nothing about
// the command-line parser, so it can be tested on its own.

#pragma once

#include <expected>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "app/game_options.h"

namespace dal {

struct ConfigFileValue {
    std::string option;               // "game-dir"
    std::string key;                  // "game_dir"
    std::vector<std::string> inputs;  // UTF-8 command-line form; one element
};

struct ConfigFileContents {
    std::vector<ConfigFileValue> values;  // document order
    std::vector<std::string> warnings;    // `unknown key "x" ignored`
};

// Parses the text of a config file. Relative Path values resolve against `base_dir` and are
// lexically normalised. Errors are one line without the file name (the caller adds it).
std::expected<ConfigFileContents, std::string> parseConfigFile(std::string_view text,
                                                               const std::filesystem::path& base_dir,
                                                               std::span<const OptionSpec> specs);

}  // namespace dal

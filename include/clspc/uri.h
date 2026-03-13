#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace clspc {

// POSIX-only: file paths are expected to be Unix-style absolute paths
std::string file_uri_from_path(const std::filesystem::path &path);
std::filesystem::path path_from_file_uri(std::string_view uri);

}  // namespace clspc

#pragma once

#include <filesystem>
#include <string_view>

namespace clspc::jdtls {

enum class Platform 
{
    Linux,
    MacOS,
};

struct InstallLayout 
{
    std::filesystem::path home;
    std::filesystem::path launcher_jar;
    std::filesystem::path config_dir;
};

Platform current_platform();
std::string_view config_dir_name(Platform platform);

std::filesystem::path find_launcher_jar(const std::filesystem::path& jdtls_home);
std::filesystem::path find_config_dir(const std::filesystem::path& jdtls_home,
                                      Platform platform = current_platform());

InstallLayout discover(const std::filesystem::path& jdtls_home,
                       Platform platform = current_platform());

}  // namespace clspc::jdtls

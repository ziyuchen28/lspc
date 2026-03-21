#pragma once

#include <filesystem>
#include <string_view>
#include <vector>

#include <pcr/proc/piped_child.h>

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

std::filesystem::path find_launcher_jar(const std::filesystem::path &jdtls_home);
std::filesystem::path find_config_dir(const std::filesystem::path &jdtls_home,
                                      Platform platform = current_platform());

InstallLayout discover(const std::filesystem::path &jdtls_home,
                       Platform platform = current_platform());

struct LaunchOptions 
{
    std::filesystem::path jdtls_home;
    std::filesystem::path workspace_dir;
    std::filesystem::path root_dir;

    std::string java_bin{"java"};
    int xms_mb{512};
    int xmx_mb{8192};

    bool log_protocol{false};
    std::string log_level{"INFO"};
};

struct CommandSpec 
{
    std::filesystem::path cwd;
    std::vector<std::string> argv;
};

CommandSpec build_command(const LaunchOptions &options,
                          Platform platform = current_platform());

pcr::proc::PipedChild spawn(const LaunchOptions &options,
                            Platform platform = current_platform());


}  // namespace clspc::jdtls

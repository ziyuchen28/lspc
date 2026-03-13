#include "clspc/jdtls.h"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

namespace clspc::jdtls {


Platform current_platform() 
{
#if defined(__APPLE__)
    return Platform::MacOS;
#else
    return Platform::Linux;
#endif
}


std::string_view config_dir_name(Platform platform) 
{
    switch (platform) {
        case Platform::Linux:
            return "config_linux";
        case Platform::MacOS:
            return "config_mac";
    }
    return "config_linux";
}


std::filesystem::path find_launcher_jar(const std::filesystem::path &jdtls_home) 
{
    const auto plugins_dir = jdtls_home / "plugins";
    if (!std::filesystem::exists(plugins_dir) || !std::filesystem::is_directory(plugins_dir)) {
        throw std::runtime_error("jdtls plugins dir not found: " + plugins_dir.string());
    }

    std::vector<std::filesystem::path> matches;
    for (const auto& entry : std::filesystem::directory_iterator(plugins_dir)) {
        if (!entry.is_regular_file()) {
            continue;
        }

        const std::string name = entry.path().filename().string();
        if (name.starts_with("org.eclipse.equinox.launcher_") &&
            entry.path().extension() == ".jar") {
            matches.push_back(entry.path());
        }
    }

    if (matches.empty()) {
        throw std::runtime_error("could not find equinox launcher jar under: " +
                                 plugins_dir.string());
    }

    std::sort(matches.begin(), matches.end());
    return std::filesystem::absolute(matches.back()).lexically_normal();
}


std::filesystem::path find_config_dir(const std::filesystem::path &jdtls_home,
                                      Platform platform) 
{
    const auto dir = jdtls_home / std::string(config_dir_name(platform));
    if (!std::filesystem::exists(dir) || !std::filesystem::is_directory(dir)) {
        throw std::runtime_error("jdtls config dir not found: " + dir.string());
    }

    return std::filesystem::absolute(dir).lexically_normal();
}


InstallLayout discover(const std::filesystem::path &jdtls_home,
                       Platform platform) 
{
    const auto home = std::filesystem::absolute(jdtls_home).lexically_normal();

    return InstallLayout{
        .home = home,
        .launcher_jar = find_launcher_jar(home),
        .config_dir = find_config_dir(home, platform),
    };
}

}  // namespace clspc::jdtls



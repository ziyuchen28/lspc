#include "clspc/jdtls.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
using namespace clspc::jdtls;

namespace {

void require(bool condition, const std::string &message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

void touch_file(const fs::path &path) {
    std::ofstream out(path);
    require(static_cast<bool>(out), "failed to create file: " + path.string());
    out << "stub";
}

}  // namespace

int main() {
    const fs::path root =
        fs::temp_directory_path() / "clspc-test-jdtls-layout";

    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "plugins", ec);
    require(!ec, "failed to create plugins dir");

    fs::create_directories(root / "config_linux", ec);
    require(!ec, "failed to create config dir");

    // Add two launcher jars so we can verify we pick the lexicographically last one.
    touch_file(root / "plugins" / "org.eclipse.equinox.launcher_1.6.900.jar");
    touch_file(root / "plugins" / "org.eclipse.equinox.launcher_1.7.100.jar");
    touch_file(root / "plugins" / "not-the-launcher.jar");

    const fs::path launcher = find_launcher_jar(root);
    require(launcher.filename() == "org.eclipse.equinox.launcher_1.7.100.jar",
            "unexpected launcher jar: " + launcher.string());

    const fs::path config = find_config_dir(root, Platform::Linux);
    require(config.filename() == "config_linux",
            "unexpected config dir: " + config.string());

    const InstallLayout layout = discover(root, Platform::Linux);
    require(layout.home == fs::absolute(root).lexically_normal(),
            "unexpected layout home");
    require(layout.launcher_jar == launcher,
            "unexpected layout launcher");
    require(layout.config_dir == config,
            "unexpected layout config");

    fs::remove_all(root, ec);

    std::cout << "test_jdtls_paths passed\n";
    return 0;
}

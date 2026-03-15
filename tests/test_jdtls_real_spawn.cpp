#include "clspc/jdtls.h"
#include "integ_test_helper.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace clspc::jdtls;


namespace {

std::string read_all_from_fd(int fd) 
{
    std::string out;
    char buf[4096];

    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n > 0) {
            out.append(buf, static_cast<std::size_t>(n));
            continue;
        }
        break;
    }

    return out;
}

bool contains_any(std::string_view text,
                  std::initializer_list<std::string_view> needles) 
{
    for (auto needle : needles) {
        if (text.find(needle) != std::string_view::npos) {
            return true;
        }
    }
    return false;
}

}  // namespace



int main() 
{
    const fs::path root =
        fs::temp_directory_path() / "clspc-test-jdtls-real-spawn";

    std::error_code ec;
    fs::remove_all(root, ec);

    const fs::path workspace = root / "workspace";
    const fs::path repo = root / "repo";
    fs::create_directories(workspace, ec);
    require(!ec, "failed to create workspace dir");
    fs::create_directories(repo, ec);
    require(!ec, "failed to create repo dir");

    // w/a to add timeout before java, which doesn't work with exec sys call
    std::string effective_java_bin = real_java_bin();
    if (auto timeout_bin = timeout_bin_from_env(); timeout_bin.has_value()) {
        const fs::path wrapper = root / "java-timebox.sh";
        {
            std::ofstream out(wrapper);
            require(static_cast<bool>(out), "failed to create wrapper script");

           out
                << "#!/usr/bin/env bash\n"
                << "set -euo pipefail\n"
                << "exec timeout 4s "
                << shell_quote_single(real_java_bin())
                << " \"$@\"\n";
            out.close();

            fs::permissions(wrapper,
                            fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write |
                            fs::perms::group_exec | fs::perms::group_read |
                            fs::perms::others_exec | fs::perms::others_read,
                            fs::perm_options::replace);
        }
        effective_java_bin = wrapper.string();
    }

    std::cout << "launch mode: " << "\n";
    std::cout << "java_bin=" << real_java_bin() << "\n";
    if (effective_java_bin == real_java_bin()) {
        std::cout << "timeout_wrapper=disabled\n";
    } else {
        std::cout << "timeout_wrapper=enabled\n";
        std::cout << "effective_java_bin=" << effective_java_bin << "\n";
    }

    LaunchOptions opt;
    opt.jdtls_home = fs::path(real_jdtls_home());
    opt.workspace_dir = workspace;
    opt.root_dir = repo;
    opt.java_bin = effective_java_bin;
    opt.log_protocol = false;
    opt.log_level = "INFO";

    auto child = spawn(opt, Platform::Linux);

    // Keep stdin open; let timeout stop the process after a few seconds.
    child.wait();

    const std::string stdout_text = read_all_from_fd(child.stdout_read_fd());
    const std::string stderr_text = read_all_from_fd(child.stderr_read_fd());

    // We expect real JDTLS/JVM startup output on stderr.
    require(contains_any(stderr_text, {
        "JavaLanguageServerPlugin is started",
        "Main thread is waiting",
        "Using incubator modules",
        "org.apache.aries.spifly.BaseActivator"
    }), "did not observe expected JDTLS/JVM startup output.\nStderr was:\n" + stderr_text);

    fs::remove_all(root, ec);

    std::cout << "test_jdtls_real_spawn passed\n";
    return 0;
}

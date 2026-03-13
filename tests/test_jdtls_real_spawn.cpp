#include "clspc/jdtls.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace clspc::jdtls;

namespace {

void require(bool condition, const std::string &message) 
{
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

std::string real_jdtls_home() 
{
    if (const char *env = std::getenv("CLSPC_TEST_JDTLS_HOME")) {
        return env;
    }
    require(false, "set CLSPC_TEST_JDTLS_HOME to the extracted JDTLS directory");
    return {};
}

std::string real_java_bin() 
{
    if (const char* env = std::getenv("CLSPC_TEST_JAVA_BIN")) {
        return env;
    }
    return "java";
}

std::string shell_quote_single(std::string_view s) 
{
    std::string out;
    out.push_back('\'');
    for (char ch : s) {
        if (ch == '\'') {
            out += "'\\''";
        } else {
            out.push_back(ch);
        }
    }
    out.push_back('\'');
    return out;
}

void write_executable_script(const fs::path &path, const std::string &contents) 
{
    std::ofstream out(path);
    require(static_cast<bool>(out), "failed to create script: " + path.string());
    out << contents;
    out.close();

    fs::permissions(path,
                    fs::perms::owner_exec | fs::perms::owner_read | fs::perms::owner_write |
                    fs::perms::group_exec | fs::perms::group_read |
                    fs::perms::others_exec | fs::perms::others_read,
                    fs::perm_options::replace);
}

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

    LaunchOptions opt;
    opt.jdtls_home = fs::path(real_jdtls_home());
    opt.workspace_dir = workspace;
    opt.root_dir = repo;
    opt.java_bin = wrapper.string();
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

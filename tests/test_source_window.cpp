#include "clspc/source_window.h"
#include "test_helper.h"

#include <filesystem>
#include <string>

namespace fs = std::filesystem;
using namespace clspc;


int main() 
{
    const fs::path root =
        fs::temp_directory_path() / "clspc-test-source-window";

    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);
    require(!ec, "failed to create temp root");

    const fs::path file = root / "Example.java";
    write_file(file, R"(package demo;

public final class Example {
    public int f() {
        return 42;
    }
}
)");

    const Range range{
        .start = Position{2, 4},  // line 3 
        .end = Position{4, 5},    // line 5 
    };

    const SourceWindow window =
        extract_source_window(file, range, 1, 1);

    require(window.path == fs::absolute(file).lexically_normal(),
            "unexpected window path");
    require(window.start_line == 2,
            "unexpected start_line");
    require(window.end_line == 6,
            "unexpected end_line");

    require(contains(window.text, "2:"),
            "expected line 2 in snippet");
    require(contains(window.text, "3: public final class Example {"),
            "expected class line in snippet");
    require(contains(window.text, "4:     public int f() {"),
            "expected method line in snippet");
    require(contains(window.text, "5:         return 42;"),
            "expected body line in snippet");
    require(contains(window.text, "6:     }"),
            "expected closing brace in snippet");

    fs::remove_all(root, ec);

    std::cout << "test_source_window passed\n";
    return 0;
}

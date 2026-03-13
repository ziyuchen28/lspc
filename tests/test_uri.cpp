#include "clspc/uri.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace fs = std::filesystem;
using namespace clspc;

namespace {

void require(bool condition, const std::string &message) 
{
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}

}  // namespace



int main() 
{
    {
        const fs::path p = "/tmp/hello.txt";
        const std::string uri = file_uri_from_path(p);

        require(uri == "file:///tmp/hello.txt",
                "path URI mismatch: " + uri);

        const fs::path roundtrip = path_from_file_uri(uri);
        require(roundtrip == fs::path("/tmp/hello.txt"),
                "path roundtrip mismatch: " + roundtrip.string());
    }

    {
        const fs::path p = "/tmp/dir with spaces/a+b#c?.java";
        const std::string uri = file_uri_from_path(p);

        require(uri == "file:///tmp/dir%20with%20spaces/a%2Bb%23c%3F.java",
                "encoded path URI mismatch: " + uri);

        const fs::path roundtrip = path_from_file_uri(uri);
        require(roundtrip == fs::path("/tmp/dir with spaces/a+b#c?.java"),
                "encoded path roundtrip mismatch: " + roundtrip.string());
    }

    {
        const fs::path roundtrip =
            path_from_file_uri("file://localhost/tmp/sample.java");
        require(roundtrip == fs::path("/tmp/sample.java"),
                "localhost URI roundtrip mismatch: " + roundtrip.string());
    }

    std::cout << "test_uri passed\n";
    return 0;
}

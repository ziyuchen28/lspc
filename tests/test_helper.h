
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>


inline void require(bool condition, const std::string &message) 
{
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        std::exit(1);
    }
}


inline void write_executable_script(const std::filesystem::path &path, const std::string &contents) 
{
    namespace fs = std::filesystem;
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


inline std::string read_file(const std::filesystem::path &path) 
{
    std::ifstream in(path);
    require(static_cast<bool>(in), "failed to open file: " + path.string());
    std::string out((std::istreambuf_iterator<char>(in)),
                    std::istreambuf_iterator<char>());
    return out;
}


inline bool contains(std::string_view haystack, std::string_view needle) 
{
    return haystack.find(needle) != std::string_view::npos;
}


inline void touch_file(const std::filesystem::path &path, const std::string &text) 
{
    std::ofstream out(path);
    require(static_cast<bool>(out), "failed to create file: " + path.string());
    out << text;
}


inline void write_file(const std::filesystem::path &path, const std::string &contents) 
{
    std::ofstream out(path);
    require(static_cast<bool>(out), "failed to create file: " + path.string());
    out << contents;
}




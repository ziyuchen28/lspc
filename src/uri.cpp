#include "clspc/uri.h"

#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace clspc {


static bool is_unreserved(unsigned char c) 
{
    if ((c >= 'a' && c <= 'z') ||
        (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9')) {
        return true;
    }

    switch (c) {
        case '-':
        case '.':
        case '_':
        case '~':
        case '/':
            return true;
        default:
            return false;
    }
}

// %xx
static std::string percent_encode(std::string_view s) 
{
    static constexpr char k_hex[] = "0123456789ABCDEF";

    std::string out;
    out.reserve(s.size() * 3);

    for (unsigned char c : s) {
        if (is_unreserved(c)) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(k_hex[(c >> 4) & 0xF]);
            out.push_back(k_hex[c & 0xF]);
        }
    }

    return out;
}

static int hex_value(char ch) 
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
    if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
    return -1;
}

static std::string percent_decode(std::string_view s) 
{
    std::string out;
    out.reserve(s.size());

    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            const int hi = hex_value(s[i + 1]);
            const int lo = hex_value(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out.push_back(static_cast<char>((hi << 4) | lo));
                i += 2;
                continue;
            }
        }
        out.push_back(s[i]);
    }

    return out;
}


// posix only
std::string file_uri_from_path(const std::filesystem::path &path) 
{
    const auto abs = std::filesystem::absolute(path).lexically_normal();
    const std::string raw = abs.string();

    if (raw.empty() || raw.front() != '/') {
        throw std::runtime_error("file_uri_from_path expects a POSIX absolute path");
    }

    return "file://" + percent_encode(raw);
}


std::filesystem::path path_from_file_uri(std::string_view uri) 
{
    if (!uri.starts_with("file://")) {
        throw std::runtime_error("not a file:// URI: " + std::string(uri));
    }

    std::string_view tail = uri.substr(std::string_view("file://").size());

    // Accept file://localhost/path as well.
    if (tail.starts_with("localhost/")) {
        tail.remove_prefix(std::string_view("localhost").size());
    }

    const std::string decoded = percent_decode(tail);
    if (decoded.empty() || decoded.front() != '/') {
        throw std::runtime_error("decoded URI is not a POSIX absolute path: " + decoded);
    }

    return std::filesystem::path(decoded).lexically_normal();
}

}  // namespace clspc



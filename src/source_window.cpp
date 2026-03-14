#include "clspc/source_window.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace clspc {


static std::vector<std::string> read_lines(const std::filesystem::path &path) 
{
    std::ifstream in(path);
    if (!in) {
        throw std::runtime_error("failed to open source file: " + path.string());
    }
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        // handle windows' \r\n
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(line);
    }

    return lines;
}


static std::string build_text(const std::vector<std::string> &lines,
                       std::size_t start_line,
                       std::size_t end_line) 
{
    std::ostringstream out;
    for (std::size_t line_no = start_line; line_no <= end_line; ++line_no) {
        out << line_no << ": " << lines[line_no - 1];
        if (line_no != end_line) {
            out << '\n';
        }
    }
    return out.str();
}


SourceWindow extract_source_window(const std::filesystem::path &path,
                                   const Range &range,
                                   std::size_t padding_before,
                                   std::size_t padding_after) 
{
    const auto abs = std::filesystem::absolute(path).lexically_normal();
    const std::vector<std::string> lines = read_lines(abs);

    if (lines.empty()) {
        return SourceWindow{
            .path = abs,
            .start_line = 0,
            .end_line = 0,
            .text = {},
        };
    }

    std::size_t anchor_start =
        range.start.line >= 0 ? static_cast<std::size_t>(range.start.line) + 1 : 1;
    std::size_t anchor_end =
        range.end.line >= 0 ? static_cast<std::size_t>(range.end.line) + 1 : anchor_start;

    if (anchor_end < anchor_start) {
        anchor_end = anchor_start;
    }

    anchor_start = std::clamp(anchor_start, std::size_t{1}, lines.size());
    anchor_end = std::clamp(anchor_end, std::size_t{1}, lines.size());

    const std::size_t start_line =
        anchor_start > padding_before ? anchor_start - padding_before : 1;
    const std::size_t end_line =
        std::min(lines.size(), anchor_end + padding_after);

    return SourceWindow{
        .path = abs,
        .start_line = start_line,
        .end_line = end_line,
        .text = build_text(lines, start_line, end_line),
    };
}


}  // namespace clspc




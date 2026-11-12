#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

// Whether `prefix` is what stands at `position`.
[[nodiscard]] constexpr bool starts_at(std::string_view text, std::size_t position,
                                       std::string_view prefix)
{
    return position <= text.size() && text.substr(position).starts_with(prefix);
}

[[nodiscard]] inline std::string lowercase(std::string_view text)
{
    std::string result(text);
    std::ranges::transform(result, result.begin(), [](unsigned char byte) {
        return static_cast<char>(std::tolower(byte));
    });
    return result;
}

// The lines of a text without their newlines. A final newline does not begin another line.
[[nodiscard]] inline std::vector<std::string_view> lines_of(std::string_view text)
{
    std::vector<std::string_view> lines;
    while (!text.empty()) {
        const auto end = text.find('\n');
        lines.push_back(text.substr(0, end));
        text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
    }
    return lines;
}

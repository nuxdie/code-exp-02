#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

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

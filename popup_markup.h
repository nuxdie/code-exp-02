#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

enum class PopupStyle { Emphasis, Strong, Heading, Link };
struct PopupSpan {
    std::size_t begin;
    std::size_t length;
    PopupStyle style;
    std::string destination{};
};
struct PopupDocument {
    std::string text;
    std::vector<bool> code_lines;
    std::vector<PopupSpan> styles;
    std::vector<std::pair<std::size_t, std::size_t>> inline_code;
};

PopupDocument popup_document(std::string_view text, bool markup);
std::vector<PopupSpan> web_links(std::string_view text);

// Text that must appear exactly as written, with no markup interpreted.
std::string popup_literal(std::string_view text);

std::string hover_documentation(std::string_view markdown);

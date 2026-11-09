#pragma once

#include <string>
#include <utility>
#include <vector>

enum class PopupStyle { Emphasis, Strong, Heading, Link };
struct PopupSpan {
    std::size_t begin;
    std::size_t length;
    PopupStyle style;
    std::string destination{};
};
struct PopupMath {
    std::size_t begin;
    std::string expression;
    bool display;
};
struct PopupDocument {
    std::string text;
    std::vector<bool> code_lines;
    std::vector<PopupSpan> styles;
    std::vector<PopupMath> math;
    std::vector<std::pair<std::size_t, std::size_t>> inline_code;
};

PopupDocument popup_document(const std::string& text, bool markup);

std::string hover_documentation(const std::string& markdown);

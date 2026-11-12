#include "source_panel.h"

#include "syntax.h"
#include "text.h"

#include <algorithm>
#include <cctype>
#include <filesystem>

std::string fold_identity(const SourcePanel& panel, int line)
{
    auto text = panel.text[line];
    if (line > 0 && text.find_first_not_of(" \t{") == std::string::npos)
        text = panel.text[line - 1];
    std::erase_if(text, [](unsigned char c) { return std::isspace(c) || c == '{'; });
    return text;
}

namespace {
void collapse_long_blocks(std::vector<FoldBlock>& folds)
{
    for (auto& block : folds) {
        if (block.last - block.first < 60)
            continue;
        const bool nested = std::ranges::any_of(folds, [&](const auto& other) {
            return other.first < block.first && other.last >= block.last;
        });
        block.collapsed = !nested;
    }
}
} // namespace

std::vector<FoldBlock> brace_folds(std::string_view text, std::string_view path)
{
    const auto name = std::filesystem::path(path).filename();
    const bool cmake = name == "CMakeLists.txt" || name.extension() == ".cmake";
    const auto kind = text_kind(path);
    std::vector<FoldBlock> folds;
    std::vector<int> open;
    HighlightState state;
    int number = -1;
    for (const std::string_view line : lines_of(text)) {
        ++number;
        for (const auto& token : style_line(line, kind, state, cmake).tokens) {
            if (token.syntax != Syntax::Plain)
                continue;
            for (auto offset = token.begin; offset < token.begin + token.length; ++offset) {
                if (line[offset] == '{')
                    open.push_back(number);
                if (line[offset] != '}' || open.empty())
                    continue;
                if (number > open.back() + 1)
                    folds.push_back({open.back(), number, false});
                open.pop_back();
            }
        }
    }
    collapse_long_blocks(folds);
    return folds;
}

int folded_rows(std::string_view text, std::string_view path)
{
    const int lines = static_cast<int>(std::ranges::count(text, '\n')) +
                      (!text.empty() && text.back() != '\n' ? 1 : 0);
    return static_cast<int>(fold_view(lines, brace_folds(text, path)).source_lines.size());
}

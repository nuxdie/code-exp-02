#pragma once

#include "lsp.h"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

struct FoldBlock {
    int first = 0;
    int last = 0;
    bool collapsed = false;
};

struct FoldView {
    std::vector<int> source_lines;
    std::vector<int> display_lines;
};

inline FoldView fold_view(int line_count, const std::vector<FoldBlock>& blocks)
{
    FoldView view;
    view.display_lines.resize(line_count, -1);
    for (int line = 0; line < line_count; ++line) {
        const int row = static_cast<int>(view.source_lines.size());
        view.source_lines.push_back(line);
        view.display_lines[line] = row;
        int end = line;
        for (const auto& block : blocks)
            if (block.collapsed && block.first == line)
                end = std::max(end, std::min(block.last, line_count - 1));
        for (int hidden = line + 1; hidden <= end; ++hidden)
            view.display_lines[hidden] = row;
        line = end;
    }
    return view;
}

inline bool reveal_line(int line, std::vector<FoldBlock>& blocks)
{
    bool changed = false;
    for (auto& block : blocks)
        if (block.collapsed && line > block.first && line <= block.last) {
            block.collapsed = false;
            changed = true;
        }
    return changed;
}

struct OutlineEntry {
    std::string name;
    int line = 0, last = 0;
};

struct SymbolNode {
    std::string name;
    int kind = 0;
    int line = 0;
    int column = 0;
    int first = 0;
    int last = 0;
    bool expanded = false;
    std::vector<SymbolNode> children;
};

inline std::vector<SymbolNode> symbol_tree(const lsp::Json& data)
{
    std::vector<SymbolNode> result;
    if (!data.is_array())
        return result;
    for (const auto& item : data) {
        const auto range = item.value("range", lsp::Json::object());
        const auto selection = item.value("selectionRange", range);
        if (!range.contains("start") || !range.contains("end"))
            continue;
        SymbolNode node;
        node.name = item.value("name", "");
        node.kind = item.value("kind", 0);
        node.line = selection["start"].value("line", 0);
        node.column = selection["start"].value("character", 0);
        node.first = range["start"].value("line", 0);
        node.last = range["end"].value("line", 0);
        node.children = symbol_tree(item.value("children", lsp::Json::array()));
        result.push_back(std::move(node));
    }
    return result;
}

namespace structure_text {
inline int indent(std::string_view row)
{
    int width = 0;
    for (const char character : row) {
        if (character != ' ' && character != '\t')
            break;
        width += character == '\t' ? 4 : 1;
    }
    return width;
}

inline bool listed(const std::vector<std::string>& words, std::string_view word)
{
    return std::ranges::find(words, word) != words.end();
}

inline std::string named_by_keyword(std::string_view row, std::size_t first)
{
    static const std::vector<std::string> kinds{
        "class",    "struct", "enum",  "namespace", "def",    "fn",   "func",
        "function", "impl",   "trait", "interface", "module", "type", "object"};
    for (const auto& kind : kinds) {
        const auto at = row.find(kind + " ", first);
        if (at == std::string::npos || (at > first && row[at - 1] != ' '))
            continue;
        const auto begin = row.find_first_not_of(' ', at + kind.size());
        if (begin == std::string::npos)
            continue;
        const auto end = row.find_first_of(" \t(:{<;,", begin);
        return std::string(row.substr(begin, end == std::string::npos ? end : end - begin));
    }
    return "";
}

inline std::string named_by_call(std::string_view row)
{
    const auto call = row.find('(');
    const auto last = row.find_last_not_of(" \t");
    if (call == std::string::npos || call == 0 ||
        std::string_view(";,(+-*/=&|<.?").contains(row[last]))
        return "";
    const auto end = row.find_last_not_of(' ', call - 1) + 1;
    auto begin = end;
    while (begin > 0 &&
           (std::isalnum(static_cast<unsigned char>(row[begin - 1])) || row[begin - 1] == '_'))
        --begin;
    return std::string(row.substr(begin, end - begin));
}

inline std::string declared(std::string_view row)
{
    static const std::vector<std::string> control{"if",   "for",    "while",  "switch", "catch",
                                                  "else", "return", "do",     "elif",   "with",
                                                  "try",  "match",  "except", "until",  "foreach"};
    const auto first = row.find_first_not_of(" \t");
    const auto head = row.substr(first, row.find_first_of(" \t(:{<", first) - first);
    if (listed(control, head))
        return "";
    const auto name = named_by_keyword(row, first);
    return name.empty() ? named_by_call(row) : name;
}

inline bool comment(std::string_view row, std::size_t first)
{
    const auto start = row.substr(first, 2);
    return start == "//" || start == "/*" || row[first] == '#' || row[first] == '*';
}
} // namespace structure_text

inline std::string enclosing_name(const std::vector<std::string>& text, int line)
{
    if (line < 0 || line >= std::ssize(text))
        return "";
    int limit = structure_text::indent(text[line]);
    for (int above = line - 1; above >= 0 && limit > 0; --above) {
        const auto& row = text[above];
        const auto first = row.find_first_not_of(" \t");
        const int depth = structure_text::indent(row);
        if (first == std::string::npos || depth >= limit || structure_text::comment(row, first))
            continue;
        const bool brace = row.find_first_not_of(" \t{") == std::string::npos;
        limit = brace ? depth + 1 : depth;
        const auto name = brace ? "" : structure_text::declared(row);
        if (!name.empty())
            return name;
    }
    return "";
}

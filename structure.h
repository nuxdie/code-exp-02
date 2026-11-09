#pragma once

#include "lsp.h"

#include <algorithm>
#include <string>
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

inline bool symbol_definition(const std::vector<SymbolNode>& nodes, int line, int column)
{
    for (const auto& node : nodes) {
        if (node.line == line && column >= node.column &&
            column <= node.column + lsp::byte_to_utf16(node.name, node.name.size()))
            return true;
        if (symbol_definition(node.children, line, column))
            return true;
    }
    return false;
}

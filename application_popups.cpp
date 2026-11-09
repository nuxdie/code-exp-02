#include "application.h"
#include "popup_markup.h"

void Application::hide_popup()
{
    if (!popup_text.empty())
        redraw = true;
    popup_text.clear();
    popup_locations.clear();
    popup_symbol_rows.clear();
    popup_row_depths.clear();
    jump_rows.clear();
    outline_panel = -1;
    list_offset = 0;
    hover_request = 0;
    list_request = 0;
    hover_due = Clock::time_point::max();
    popup_dismiss_due = Clock::time_point::max();
}

void Application::show_popup(std::string text, Vec2 origin, bool syntax)
{
    auto document = popup_document(text, syntax);
    popup_text = std::move(document.text);
    popup_code_lines = std::move(document.code_lines);
    popup_inline_code = std::move(document.inline_code);
    popup_prose = syntax;
    popup_styles = std::move(document.styles);
    popup_math = std::move(document.math);
    popup_scroll = 0;
    cached_popup.clear();
    popup_origin = origin;
    popup_dismiss_due = Clock::time_point::max();
    redraw = true;
}

void Application::refresh_list()
{
    list_offset = std::clamp(list_offset, 0, std::max(0, static_cast<int>(jump_rows.size()) - 12));
    std::string text = list_title;
    popup_locations.clear();
    popup_symbol_rows.clear();
    popup_row_depths.clear();
    for (int i = list_offset; i < std::min(static_cast<int>(jump_rows.size()), list_offset + 12);
         ++i) {
        const auto& row = jump_rows[i];
        text += "\n" + row.label;
        popup_locations.push_back(row.location);
        popup_symbol_rows.push_back(row.node);
        popup_row_depths.push_back(row.depth);
    }
    if (jump_rows.empty())
        text += "\nNo symbols or locations yet.";
    show_popup(text, popup_origin);
}

void Application::show_outline(int index)
{
    if (index < 0 || index >= static_cast<int>(panels.size()))
        return;
    auto& panel = panels[index];
    auto* server = server_for(panel);
    if (server && opened_documents.insert(panel.uri).second)
        server->open({panel.uri, panel.source});
    outline_panel = index;
    list_title = "Symbols · " + panel.path;
    jump_rows.clear();
    auto visit = [&](auto&& self, std::vector<SymbolNode>& nodes, int depth) -> void {
        for (auto& node : nodes) {
            std::string marker = node.children.empty() ? "  " : node.expanded ? "▾ " : "▸ ";
            const auto label = std::string(depth * 2, ' ') + marker + node.name;
            lsp::Json location = {{"uri", panel.uri},
                                  {"range",
                                   {{"start", {{"line", node.line}, {"character", node.column}}},
                                    {"end", {{"line", node.line}, {"character", node.column}}}}}};
            jump_rows.push_back({label, location, &node, depth});
            if (node.expanded)
                self(self, node.children, depth + 1);
        }
    };
    visit(visit, panel.symbols, 0);
    refresh_list();
}

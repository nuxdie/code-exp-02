#include "application.h"
#include "intent_document.h"

namespace {
std::string bounded_text(std::string value, std::size_t limit)
{
    if (value.size() <= limit)
        return value;
    while (limit && (static_cast<unsigned char>(value[limit]) & 0xc0) == 0x80)
        --limit;
    value.resize(limit);
    return value + "\n…";
}
std::string containing_symbol(const std::vector<SymbolNode>& symbols, int line)
{
    for (const auto& symbol : symbols) {
        if (line < symbol.first || line > symbol.last)
            continue;
        const auto child = containing_symbol(symbol.children, line);
        return symbol.name + (child.empty() ? "" : "::" + child);
    }
    return {};
}
std::string source_excerpt(const SourcePanel& source, int line)
{
    std::string text;
    const int first = std::max(0, line - 5);
    const int last = std::min(static_cast<int>(source.text.size()), line + 8);
    for (int i = first; i < last; ++i)
        text += std::to_string(i + 1) + ": " + source.text[i] + "\n";
    return bounded_text(std::move(text), 6000);
}
} // namespace
lsp::Json Application::agent_context(Vec2 pointer)
{
    auto hit = hit_test(pointer);
    if (hit.panel < 0) {
        const auto index = panel_at(pointer);
        if (index >= 0)
            hit = {index, 0, 0};
    }
    const auto selected = bounded_text(selected_text(), 12000);
    if (!selected.empty() && text_selection.surface == TextSurface::Source) {
        const auto found = std::find_if(panels.begin(), panels.end(), [&](const auto& panel) {
            return panel.path == text_selection.key;
        });
        if (found != panels.end()) {
            const auto begin = text_selection.range.selection().first;
            const auto line =
                std::count(found->source.begin(),
                           found->source.begin() + std::min(begin, found->source.size()), '\n');
            hit = {static_cast<int>(found - panels.begin()), static_cast<int>(line), 0};
        }
    }
    lsp::Json context{{"workspace", workspace_root},
                      {"selected_text", selected},
                      {"note", "Canvas context is a location and text snapshot. Read current files "
                               "before making changes."}};
    const auto world = camera.screen_to_world(pointer, viewport());
    context["position"] = {{"x", world.x}, {"y", world.y}};
    if (hit.panel >= 0) {
        const auto& source = panels[hit.panel];
        context["path"] = source.path;
        context["line"] = hit.line + 1;
        context["symbol"] = containing_symbol(source.symbols, hit.line);
        context["excerpt"] = source_excerpt(source, hit.line);
        context["source_version"] = source.version;
    } else {
        const CanvasRect visible{camera.screen_to_world({0, 0}, viewport()),
                                 {width / camera.zoom, height / camera.zoom}};
        context["visible_files"] = lsp::Json::array();
        for (const auto& source : panels)
            if (source.bounds.intersects(visible) && context["visible_files"].size() < 6)
                context["visible_files"].push_back(source.path);
    }
    return context;
}
void Application::refresh_agent_context()
{
    if (chat->context.empty())
        return;
    const auto path = chat->context.value("path", "");
    const auto found = std::find_if(panels.begin(), panels.end(),
                                    [&](const auto& panel) { return panel.path == path; });
    if (found != panels.end()) {
        const auto line = std::clamp(chat->context.value("line", 1) - 1, 0,
                                     std::max(0, static_cast<int>(found->text.size()) - 1));
        chat->context["excerpt"] = source_excerpt(*found, line);
        chat->context["source_version"] = found->version;
    }
    chat->conversation.canvas_context = chat->context.dump(2);
    render_agent_context();
}
void Application::render_agent_context()
{
    if (chat->context.empty())
        return;
    auto& entries = chat->conversation.entries;
    auto found = std::find_if(entries.begin(), entries.end(),
                              [](const auto& entry) { return entry.id == "canvas-context"; });
    if (found == entries.end()) {
        ConversationEntry entry;
        entry.id = "canvas-context";
        entry.role = "Context";
        entries.insert(entries.begin(), std::move(entry));
        found = entries.begin();
    }
    const auto path = chat->context.value("path", "");
    std::string text = path.empty()
                           ? "This part of the canvas"
                           : "[" + intent_literal(path) + ":" +
                                 std::to_string(chat->context.value("line", 1)) + "](" + path +
                                 ":" + std::to_string(chat->context.value("line", 1)) + ")";
    const auto symbol = chat->context.value("symbol", "");
    if (!symbol.empty())
        text += "\n" + intent_literal(symbol);
    const auto selected = chat->context.value("selected_text", "");
    if (!selected.empty())
        text += "\n\n" + intent_literal(bounded_text(selected, 500));
    if (found->text != text) {
        found->text = std::move(text);
        ++found->revision;
    }
}

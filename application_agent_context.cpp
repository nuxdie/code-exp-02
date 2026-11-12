#include "application.h"

#include <cstdint>
#include <string_view>

namespace {
std::uint64_t selection_fingerprint(std::string_view text)
{
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : text) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}
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
std::string preview_excerpt(std::string text)
{
    std::size_t end = 0;
    for (int line = 0; line < 6; ++line) {
        end = text.find('\n', end);
        if (end == std::string::npos)
            return bounded_text(std::move(text), 600);
        ++end;
    }
    return bounded_text(std::move(text), std::min(end, std::size_t{600}));
}
std::string context_preview_text(const lsp::Json& context)
{
    if (context.empty())
        return {};
    std::string text = "Context included";
    const auto path = context.value("path", "");
    if (!path.empty()) {
        text += "\n" + path + ":" + std::to_string(context.value("line", 1));
        const auto symbol = context.value("symbol", "");
        if (!symbol.empty())
            text += " · " + symbol;
    } else {
        const auto workspace = context.value("workspace", "");
        if (!workspace.empty())
            text += "\nWorkspace: " + workspace;
        const auto files = context.value("visible_files", lsp::Json::array());
        if (!files.empty()) {
            text += "\nVisible files: ";
            for (std::size_t i = 0; i < files.size(); ++i)
                text += (i ? ", " : "") + files[i].get<std::string>();
        }
    }
    const auto selected = context.value("selected_text", "");
    const auto excerpt = context.value("excerpt", "");
    if (path.empty() && !selected.empty())
        text += "\nSelected text:\n" + preview_excerpt(selected);
    else if (path.empty() && !excerpt.empty())
        text += "\nSource excerpt:\n" + preview_excerpt(excerpt);
    return text;
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
        if (!selected.empty() && text_selection.surface == TextSurface::Source &&
            text_selection.key == source.path) {
            const auto [begin, end] = text_selection.range.selection();
            context["selection_range"] = {
                {"begin", begin},
                {"end", end},
                {"source_version", source.version},
                {"fingerprint", selection_fingerprint(
                                    std::string_view(source.source).substr(begin, end - begin))}};
        }
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
std::optional<std::pair<std::size_t, std::size_t>>
Application::prompt_source_range(const SourcePanel& panel) const
{
    if (!chat || !chat->visible || !chat->composing ||
        chat->context.value("path", "") != panel.path)
        return std::nullopt;
    const auto& context = chat->context;
    if (context.contains("selection_range")) {
        const auto& range = context["selection_range"];
        const auto begin = range.value("begin", std::size_t{0});
        const auto end = range.value("end", std::size_t{0});
        if (begin >= end || end > panel.source.size())
            return std::nullopt;
        const bool matches =
            range.contains("fingerprint")
                ? range["fingerprint"] ==
                      selection_fingerprint(
                          std::string_view(panel.source).substr(begin, end - begin))
                : range.value("source_version", 0) == panel.version;
        if (!matches)
            return std::nullopt;
        return std::pair{begin, end};
    }
    const int line = context.value("line", 1) - 1;
    if (context.value("source_version", panel.version) != panel.version || line < 0 ||
        line >= static_cast<int>(panel.text.size()))
        return std::nullopt;
    std::size_t begin = 0;
    for (int i = 0; i < line; ++i)
        begin += panel.text[i].size() + 1;
    return std::pair{begin, begin + panel.text[line].size() + 1};
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
    if (chat->composing)
        prepare_chat_prompt();
}
void Application::prepare_chat_prompt()
{
    const auto text = context_preview_text(chat->context);
    double preview_height = 0;
    if (!text.empty()) {
        update_document(chat->context_preview, text, static_cast<int>(chat->bounds.size.x - 24),
                        false);
        auto& preview = chat->context_preview;
        if (preview.size.y > 240 &&
            pango_layout_get_ellipsize(preview.layout.get()) == PANGO_ELLIPSIZE_NONE) {
            pango_layout_set_height(preview.layout.get(), 240 * PANGO_SCALE);
            pango_layout_set_ellipsize(preview.layout.get(), PANGO_ELLIPSIZE_END);
            renderer->upload(preview.mesh, {{preview.layout.get(), {0, 0}}});
        }
        preview_height = std::min(240.0, chat->context_preview.size.y) + 24;
    }
    double feedback_height = 0;
    if (!chat->conversation.error.empty()) {
        update_document(chat->status, chat->conversation.error,
                        static_cast<int>(chat->bounds.size.x - 24), false);
        feedback_height = chat->status.size.y + 24;
    }
    chat->bounds.size.y = 90 + preview_height + feedback_height;
    chat->body = {};
    chat->editor = {{chat->bounds.origin.x, chat->bounds.origin.y + preview_height},
                    {chat->bounds.size.x, 90}};
}
void Application::render_agent_context()
{
    auto& entries = chat->conversation.entries;
    auto found = std::find_if(entries.begin(), entries.end(),
                              [](const auto& entry) { return entry.id == "canvas-context"; });
    const auto path = chat->context.value("path", "");
    std::string text;
    if (!path.empty())
        text = "[" + popup_literal(path) + ":" + std::to_string(chat->context.value("line", 1)) +
               "](" + path + ":" + std::to_string(chat->context.value("line", 1)) + ")";
    const auto symbol = chat->context.value("symbol", "");
    if (!symbol.empty())
        text += (text.empty() ? "" : "\n") + popup_literal(symbol);
    const auto selected = chat->context.value("selected_text", "");
    if (!selected.empty())
        text += (text.empty() ? "" : "\n\n") + popup_literal(bounded_text(selected, 500));
    if (text.empty()) {
        if (found != entries.end())
            entries.erase(found);
        return;
    }
    if (found == entries.end()) {
        ConversationEntry entry;
        entry.id = "canvas-context";
        entry.role = "Context";
        entries.insert(entries.begin(), std::move(entry));
        found = entries.begin();
    }
    if (found->text != text) {
        found->text = std::move(text);
        ++found->revision;
    }
}

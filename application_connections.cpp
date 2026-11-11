#include "application.h"

namespace {
GpuPath connection_curve(Vec2 start, Vec2 end, std::vector<Vec2> controls = {})
{
    GpuPath path;
    path.color = {0.25f, 0.42f, 0.68f, 0.46f};
    path.width = 1.3f;
    if (controls.size() < 4)
        controls = {start, {start.x + 48, start.y}, {end.x - 48, end.y}, end};
    else {
        // Retain Graphviz's route, adjusting it to the live text positions at both ends.
        const Vec2 a{start.x - controls.front().x, start.y - controls.front().y};
        const Vec2 b{end.x - controls.back().x, end.y - controls.back().y};
        for (std::size_t i = 0; i < controls.size(); ++i) {
            const double t = static_cast<double>(i) / (controls.size() - 1);
            controls[i].x += a.x * (1 - t) + b.x * t;
            controls[i].y += a.y * (1 - t) + b.y * t;
        }
        controls.front() = start;
        controls.back() = end;
    }
    for (std::size_t i = 0; i + 3 < controls.size(); i += 3) {
        for (int sample = 0; sample <= 16; ++sample) {
            const double t = sample / 16.0, s = 1 - t;
            path.points.push_back(
                {s * s * s * controls[i].x + 3 * s * s * t * controls[i + 1].x +
                     3 * s * t * t * controls[i + 2].x + t * t * t * controls[i + 3].x,
                 s * s * s * controls[i].y + 3 * s * s * t * controls[i + 1].y +
                     3 * s * t * t * controls[i + 2].y + t * t * t * controls[i + 3].y});
        }
    }
    return path;
}

} // namespace

Vec2 Application::dependency_anchor(const SourcePanel& panel, int line, std::size_t byte,
                                    bool outgoing)
{
    if (panel.folded.source_lines.empty())
        return {panel.bounds.origin.x + 24, panel.bounds.origin.y + header_height};
    line = std::clamp(line, 0, static_cast<int>(panel.text.size()) - 1);
    const int row = panel.folded.display_lines[line];
    const int visible = panel.folded.source_lines[row];
    if (visible != line)
        byte = panel.text[visible].size();
    byte = std::min(byte, panel.text[visible].size());
    PangoRectangle glyph{};
    pango_layout_index_to_pos(panel.lines[visible], static_cast<int>(byte), &glyph);
    return {panel.bounds.origin.x + 24 + static_cast<double>(glyph.x) / PANGO_SCALE +
                (outgoing ? 8 : -8),
            panel.bounds.origin.y + header_height + (row + 0.5) * line_height};
}

GpuPath Application::dependency_path(std::size_t index)
{
    const auto& edge = dependencies[index];
    if (edge.source < 0 || edge.target < 0 || edge.source >= static_cast<int>(panels.size()) ||
        edge.target >= static_cast<int>(panels.size()))
        return {};
    const auto& target = panels[edge.target];
    const auto first = std::find_if(target.text.begin(), target.text.end(), [](const auto& line) {
        return line.find_first_not_of(" \t\r") != std::string::npos;
    });
    const int line = first == target.text.end() ? 0 : static_cast<int>(first - target.text.begin());
    const auto byte = first == target.text.end() ? 0 : first->find_first_not_of(" \t\r");
    const Vec2 start = dependency_anchor(panels[edge.source], edge.line, edge.byte, true);
    const Vec2 end = dependency_anchor(target, line, byte, false);
    std::vector<Vec2> controls;
    if (index < graph.routes.size())
        controls = graph.routes[index];
    auto path = connection_curve(start, end, std::move(controls));
    path.color = {0.42f, 0.49f, 0.59f, 0.24f};
    path.width = 1;
    return path;
}

void Application::rebuild_connections()
{
    std::vector<GpuPath> paths;
    std::set<std::string> connected;
    for (const auto& edge : symbol_graph.connections()) {
        auto path = symbol_path(edge);
        if (path.points.empty())
            continue;
        connected.insert(edge.source.uri);
        paths.push_back(std::move(path));
    }
    for (std::size_t index = 0; index < dependencies.size(); ++index) {
        const auto& edge = dependencies[index];
        if (!edge.build && edge.source >= 0 && edge.source < static_cast<int>(panels.size()) &&
            connected.count(panels[edge.source].uri))
            continue;
        auto path = dependency_path(index);
        paths.push_back(std::move(path));
    }
    renderer->paths(connection_mesh, paths);
    connections_dirty = false;
}

GpuPath Application::symbol_path(const SymbolConnection& edge)
{
    const auto source = std::find_if(panels.begin(), panels.end(), [&](const auto& panel) {
        return panel.uri == edge.source.uri;
    });
    const auto target = std::find_if(panels.begin(), panels.end(), [&](const auto& panel) {
        return panel.uri == edge.target_uri;
    });
    if (source == panels.end() || target == panels.end() ||
        source->version != edge.source.version || edge.source.line < 0 || edge.line < 0 ||
        edge.source.line >= static_cast<int>(source->text.size()) ||
        edge.line >= static_cast<int>(target->text.size()))
        return {};
    const auto byte = lsp::utf16_to_byte(target->text[edge.line], edge.column);
    if (source == target &&
        ((edge.source.line == edge.line && byte >= edge.source.begin && byte < edge.source.end) ||
         source->folded.display_lines[edge.source.line] == target->folded.display_lines[edge.line]))
        return {};
    const auto start = dependency_anchor(*source, edge.source.line, edge.source.end, true);
    const auto end = dependency_anchor(*target, edge.line, byte, false);
    if (source != target)
        return connection_curve(start, end);
    const double gutter = source->bounds.origin.x + source->bounds.size.x + 40;
    return connection_curve(start, end, {start, {gutter, start.y}, {gutter, end.y}, end});
}

void Application::request_symbol_connections()
{
    if (!language_ready || !language_server)
        return;
    while (const auto query = symbol_graph.next()) {
        const auto panel = std::find_if(panels.begin(), panels.end(),
                                        [&](const auto& entry) { return entry.uri == query->uri; });
        if (panel == panels.end() || panel->version != query->version ||
            query->line >= static_cast<int>(panel->text.size()))
            continue;
        const lsp::Json params{
            {"textDocument", {{"uri", query->uri}}},
            {"position",
             {{"line", query->line},
              {"character", lsp::byte_to_utf16(panel->text[query->line], query->begin)}}}};
        const int id = language_server->request("textDocument/definition", query->uri, params);
        symbol_graph.started(id, *query);
    }
}

void Application::reset_symbol_connections()
{
    std::set<std::string> documents;
    for (const auto& panel : panels)
        documents.insert(panel.uri);
    symbol_graph.reset(std::move(documents));
    for (const auto& panel : panels)
        if (panel.cpp)
            symbol_graph.update(panel.uri, panel.version, panel.semantics, declaration_modifiers);
    request_symbol_connections();
    connections_dirty = true;
}

void Application::refresh_symbol_sources(const std::vector<ProjectChange>& changes)
{
    if (!language_server)
        return;
    std::set<std::string> changed;
    const std::set<std::string> extensions{".c", ".cc", ".cpp", ".cxx", ".h", ".hpp"};
    for (const auto& change : changes)
        if (extensions.count(std::filesystem::path(change.path).extension().string()))
            changed.insert(change.path);
    if (changed.empty())
        return;
    // clangd can retain an unchanged caller's AST after its included header has moved.
    // Reparse open documents before resolving their uses against the new definitions.
    for (auto& panel : panels) {
        if (!panel.cpp || !opened_documents.count(panel.uri) || changed.count(panel.path))
            continue;
        ++panel.version;
        language_server->change({panel.uri, panel.source});
    }
}

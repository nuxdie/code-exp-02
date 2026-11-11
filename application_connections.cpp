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

namespace {
constexpr double reading_zoom = 0.35;
constexpr std::array<float, 4> uses_color{0.20f, 0.45f, 0.85f, 0.9f};
constexpr std::array<float, 4> used_by_color{0.85f, 0.50f, 0.15f, 0.9f};

const SymbolNode* innermost_symbol(const std::vector<SymbolNode>& nodes, int line, bool callable)
{
    for (const auto& node : nodes) {
        if (line < node.first || line > node.last)
            continue;
        if (const auto* inner = innermost_symbol(node.children, line, callable))
            return inner;
        // LSP kinds: 6 method, 9 constructor, 12 function.
        if (!callable || node.kind == 6 || node.kind == 9 || node.kind == 12)
            return &node;
    }
    return nullptr;
}

// The unit a reader thinks in around a line: its function, else its block, else the line.
std::pair<int, int> enclosing_unit(const SourcePanel& panel, int line)
{
    if (const auto* symbol = innermost_symbol(panel.symbols, line, true))
        return {symbol->first, symbol->last};
    std::pair<int, int> unit{line, line};
    auto opens_alone = [&](int first) {
        return first > 0 && panel.text[first].find_first_not_of(" \t{") == std::string::npos;
    };
    const int limit = static_cast<int>(panel.text.size()) * 3 / 5;
    for (const auto& fold : panel.folds)
        if (line >= fold.first && line <= fold.last && fold.last - fold.first < limit &&
            fold.last - fold.first > unit.second - unit.first)
            unit = {fold.first, fold.last};
    // A brace on its own line belongs to the declaration above it.
    if (opens_alone(unit.first))
        --unit.first;
    return unit;
}

Vec2 card_port(const CanvasRect& card, const CanvasRect& other)
{
    const bool right = other.origin.x + other.size.x / 2 > card.origin.x + card.size.x / 2;
    return {card.origin.x + (right ? card.size.x + 6 : -6),
            card.origin.y + std::min(card.size.y / 2, 240.0)};
}
} // namespace

// Arrows answer a question about what the pointer is on; they are never all shown at once.
Application::ConnectionFocus Application::connection_focus_at(Vec2 pointer)
{
    if (drag_button || search.active || chat_at(pointer))
        return connection_focus;
    for (std::size_t i = 0; i < header_hits.size() && i < panels.size(); ++i)
        if (CanvasWindowManager::contains(header_hits[i], pointer))
            return {static_cast<int>(i), -1, -1};
    const auto hit = source_line_at(pointer);
    if (!hit)
        return {};
    if (camera.zoom < reading_zoom)
        return {hit->panel, -1, -1};
    const auto unit = enclosing_unit(panels[hit->panel], hit->line);
    return {hit->panel, unit.first, unit.second};
}

// One arrow per pair of files, for a whole file under the pointer.
void Application::file_connections(std::vector<GpuPath>& paths)
{
    std::map<std::string, int> index;
    for (std::size_t i = 0; i < panels.size(); ++i)
        index[panels[i].uri] = static_cast<int>(i);
    std::map<std::pair<int, int>, int> pairs;
    for (const auto& edge : symbol_graph.connections()) {
        const auto source = index.find(edge.source.uri), target = index.find(edge.target_uri);
        if (source != index.end() && target != index.end() && source->second != target->second)
            ++pairs[{source->second, target->second}];
    }
    const int count = static_cast<int>(panels.size());
    for (const auto& edge : dependencies)
        if (edge.source >= 0 && edge.target >= 0 && edge.source < count && edge.target < count &&
            edge.source != edge.target)
            ++pairs[{edge.source, edge.target}];
    const int focus = connection_focus.panel;
    for (const auto& [pair, uses] : pairs) {
        if (pair.first != focus && pair.second != focus)
            continue;
        const auto& source = panels[pair.first].bounds;
        const auto& target = panels[pair.second].bounds;
        auto path = connection_curve(card_port(source, target), card_port(target, source));
        path.width = static_cast<float>(1 + std::min(3.0, std::log2(1.0 + uses) / 2));
        path.color = pair.first == focus ? uses_color : used_by_color;
        paths.push_back(std::move(path));
    }
}

// What the code under the pointer uses, and what uses it.
void Application::unit_connections(std::vector<GpuPath>& paths)
{
    const auto& focus = connection_focus;
    const auto& panel = panels[focus.panel];
    auto inside = [&](const std::string& uri, int line) {
        return uri == panel.uri && line >= focus.first && line <= focus.last;
    };
    bool resolved = false;
    for (const auto& edge : symbol_graph.connections()) {
        const bool uses = inside(edge.source.uri, edge.source.line);
        const bool used = inside(edge.target_uri, edge.line);
        resolved = resolved || edge.source.uri == panel.uri;
        if (uses == used) // unrelated, or a reference within the unit itself
            continue;
        auto path = symbol_path(edge);
        if (path.points.empty())
            continue;
        path.color = uses ? uses_color : used_by_color;
        path.width = 1.6f;
        paths.push_back(std::move(path));
    }
    // Without resolved symbols, fall back to the file-level links written on these lines.
    for (std::size_t i = 0; i < dependencies.size() && !resolved; ++i) {
        const auto& edge = dependencies[i];
        if (edge.source != focus.panel || edge.line < focus.first || edge.line > focus.last)
            continue;
        auto path = dependency_path(i);
        path.color = uses_color;
        paths.push_back(std::move(path));
    }
}

void Application::rebuild_connections()
{
    std::vector<GpuPath> paths;
    const auto& focus = connection_focus;
    if (focus.panel >= static_cast<int>(panels.size()))
        connection_focus = {};
    if (focus.panel >= 0 && focus.first >= 0)
        unit_connections(paths);
    else if (focus.panel >= 0)
        file_connections(paths);
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

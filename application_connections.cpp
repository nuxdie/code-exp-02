#include "application.h"

#include <set>

GpuPath connection_curve(Vec2 start, Vec2 end)
{
    GpuPath path;
    path.color = {0.25f, 0.42f, 0.68f, 0.46f};
    path.width = 1.3f;
    const std::vector<Vec2> controls{start, {start.x + 48, start.y}, {end.x - 48, end.y}, end};
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

// The box a word occupies on the canvas, or its fold row's end when the line is folded away.
CanvasRect Application::word_bounds(const SourcePanel& panel, int line, std::size_t begin,
                                    std::size_t end)
{
    if (panel.folded.source_lines.empty() || panel.text.empty())
        return {};
    line = std::clamp(line, 0, static_cast<int>(panel.text.size()) - 1);
    const int row = panel.folded.display_lines[line];
    const int visible = panel.folded.source_lines[row];
    const auto size = panel.text[visible].size();
    if (visible != line)
        begin = end = size;
    PangoRectangle first{}, last{};
    pango_layout_index_to_pos(panel.lines[visible].get(), static_cast<int>(std::min(begin, size)),
                              &first);
    pango_layout_index_to_pos(panel.lines[visible].get(), static_cast<int>(std::min(end, size)),
                              &last);
    const double left = static_cast<double>(first.x) / PANGO_SCALE;
    const double right = static_cast<double>(last.x) / PANGO_SCALE;
    return {{panel.bounds.origin.x + 24 + left - 2,
             panel.bounds.origin.y + header_height + row * line_height},
            {std::max(6.0, right - left) + 4, static_cast<double>(line_height)}};
}

namespace {
const SymbolNode* enclosing_function(const std::vector<SymbolNode>& nodes, int line)
{
    for (const auto& node : nodes) {
        if (line < node.first || line > node.last)
            continue;
        if (const auto* inner = enclosing_function(node.children, line))
            return inner;
        // LSP kinds: 6 method, 9 constructor, 12 function.
        if (node.kind == 6 || node.kind == 9 || node.kind == 12)
            return &node;
    }
    return nullptr;
}

// What to call a place that uses the subject: the function it sits in, from the language
// server when it has outlined the file, else read from the text, else the line number.
std::string use_name(const SourcePanel& panel, int line)
{
    if (const auto* function = enclosing_function(panel.symbols, line))
        return function->name.substr(function->name.rfind(':') + 1);
    const auto name = enclosing_name(panel.text, line);
    return name.empty() ? std::format("line {}", line + 1) : name;
}

std::string file_name(const std::string& path)
{
    return std::filesystem::path(path).filename().string();
}
} // namespace

// A click on a symbol makes it the subject: arrows then show where it is defined and
// every place that uses it. A click elsewhere in the text puts the arrows away.
void Application::show_connections_at(Vec2 pointer)
{
    clear_connections();
    const auto hit = hit_test(pointer);
    if (hit.panel < 0 || hit.line < 0)
        return;
    const auto& panel = panels[hit.panel];
    const auto word = word_at(panel.text[hit.line], hit.byte);
    if (word.first == word.second)
        return;
    connection_subject = {{panel.uri, hit.line, word.first, word.second},
                          panel.text[hit.line].substr(word.first, word.second - word.first),
                          file_name(panel.path),
                          true};
    mouse_position = pointer;
    connection_requests = {query_at_pointer("textDocument/definition"),
                           query_at_pointer("textDocument/references")};
    // Files no language server understands still have words: show where else this one occurs.
    if (!connection_requests.first && !connection_requests.second) {
        hide_popup();
        text_connections();
    }
    connections_dirty = true;
}

bool Application::clear_connections()
{
    const bool shown = connection_subject.active;
    connection_subject = {};
    connection_links.clear();
    connection_requests = {0, 0};
    connections_dirty = true;
    redraw = true;
    return shown;
}

// The same place named twice is one link with a count.
void Application::add_connection_link(ConnectionLink link)
{
    if (link.at == connection_subject.at)
        return;
    for (auto& known : connection_links)
        if (known.defines == link.defines && known.at.uri == link.at.uri &&
            known.name == link.name && (!link.defines || known.at == link.at)) {
            ++known.count;
            return;
        }
    connection_links.push_back(std::move(link));
}

// Every other whole-word occurrence of the subject's name, in any kind of file.
void Application::text_connections()
{
    const auto& name = connection_subject.name;
    for (const auto& panel : panels)
        for (std::size_t line = 0; line < panel.text.size() && connection_links.size() < 200;
             ++line)
            for (auto at = panel.text[line].find(name); at != std::string::npos;
                 at = panel.text[line].find(name, at + name.size())) {
                if (word_at(panel.text[line], at) != std::pair{at, at + name.size()})
                    continue;
                add_connection_link({{panel.uri, static_cast<int>(line), at, at + name.size()},
                                     use_name(panel, static_cast<int>(line)),
                                     file_name(panel.path),
                                     false});
            }
}

// A language server's answer: where the subject is defined, or the places that use it.
bool Application::receive_connections(const lsp::Message& message)
{
    const bool defines = message.id == connection_requests.first;
    if (!message.id || (!defines && message.id != connection_requests.second))
        return false;
    (defines ? connection_requests.first : connection_requests.second) = 0;
    const auto locations = message.result.is_array()    ? message.result
                           : message.result.is_object() ? lsp::Json::array({message.result})
                                                        : lsp::Json::array();
    for (const auto& location : locations) {
        const auto uri = location.value("uri", location.value("targetUri", ""));
        const auto range = location.contains("targetSelectionRange")
                               ? location["targetSelectionRange"]
                               : location.value("range", lsp::Json::object());
        const auto panel = std::ranges::find(panels, uri, &SourcePanel::uri);
        const int line = range.value("start", lsp::Json::object()).value("line", -1);
        if (panel == panels.end() || line < 0 || line >= std::ssize(panel->text))
            continue;
        const auto& text = panel->text[line];
        const auto word =
            word_at(text, lsp::utf16_to_byte(text, range["start"].value("character", 0)));
        add_connection_link({{uri, line, word.first, std::max(word.second, word.first + 1)},
                             defines ? connection_subject.name : use_name(*panel, line),
                             file_name(panel->path),
                             defines});
    }
    connections_dirty = true;
    redraw = true;
    return true;
}

void Application::rebuild_connections()
{
    connections_dirty = false;
    layout_connections();
}

void Application::refresh_symbol_sources(const std::vector<ProjectChange>& changes)
{
    if (!language_server)
        return;
    std::set<std::string> changed;
    const std::set<std::string> extensions{".c", ".cc", ".cpp", ".cxx", ".h", ".hpp"};
    for (const auto& change : changes)
        if (extensions.contains(std::filesystem::path(change.path).extension().string()))
            changed.insert(change.path);
    if (changed.empty())
        return;
    // clangd can retain an unchanged caller's AST after its included header has moved.
    // Reparse open documents before resolving their uses against the new definitions.
    for (auto& panel : panels) {
        if (!panel.cpp || !opened_documents.contains(panel.uri) || changed.contains(panel.path))
            continue;
        ++panel.version;
        language_server->change({panel.uri, panel.source});
    }
}

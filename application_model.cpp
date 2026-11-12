#include "application.h"

namespace {
std::optional<std::size_t> source_line_hit(PangoLayout* layout, const std::string& text, double x,
                                           int font_height)
{
    int index = 0, trailing = 0;
    if (!pango_layout_xy_to_index(layout, static_cast<int>(x * PANGO_SCALE),
                                  font_height * PANGO_SCALE / 2, &index, &trailing))
        return {};
    if (index < 0 || index >= std::ssize(text) ||
        std::isspace(static_cast<unsigned char>(text[index])))
        return {};
    return static_cast<std::size_t>(index);
}
} // namespace

SourcePanel Application::make_panel(const std::string& path, const std::string& text,
                                    ImagePreview* preview)
{
    SourcePanel panel;
    panel.path = path;
    panel.image = project_image(path);
    panel.source = panel.image ? std::string{} : text;
    panel.uri = lsp::file_uri((std::filesystem::path(workspace_root) / path).string());
    const auto extension = std::filesystem::path(path).extension().string();
    panel.cpp = extension == ".cpp" || extension == ".cc" || extension == ".cxx" ||
                extension == ".c" || extension == ".h" || extension == ".hpp";
    panel.title = make_line(path, {{0, path.size(), Syntax::Directive}});
    int text_width = 0;
    pango_layout_get_pixel_size(panel.title.get(), &text_width, nullptr);
    panel.cmake =
        std::filesystem::path(path).filename() == "CMakeLists.txt" || extension == ".cmake";
    if (panel.image) {
        load_image_panel(panel, preview ? std::move(*preview) : decode_image(text), text_width);
        return panel;
    }
    HighlightState highlight_state;
    const auto kind = text_kind(path);
    for (const std::string_view line : lines_of(text)) {
        const auto styled = style_line(line, kind, highlight_state, panel.cmake);
        const auto& tokens = styled.tokens;
        auto layout = make_line(line, tokens, styled.prose ? &*styled.prose : nullptr);
        int line_width = 0;
        pango_layout_get_pixel_size(layout.get(), &line_width, nullptr);
        text_width = std::max(text_width, line_width);
        panel.lexical.emplace_back(pango_attr_list_ref(pango_layout_get_attributes(layout.get())));
        panel.lines.push_back(std::move(layout));
        panel.text.emplace_back(line);
    }
    panel.bounds.size = {static_cast<double>(text_width + 4),
                         header_height + static_cast<double>(panel.lines.size()) * line_height};
    panel.folds = brace_folds(text, path);
    return panel;
}

void Application::update_mesh(SourcePanel& panel)
{
    connections_dirty = true;
    panel.folded = fold_view(static_cast<int>(panel.lines.size()), panel.folds);
    panel.fold_labels.clear();
    std::vector<PlacedText> text;
    text.reserve(panel.lines.size() + 1);
    for (std::size_t i = 0; i < panel.folded.source_lines.size(); ++i) {
        const int source = panel.folded.source_lines[i];
        const double y = header_height + static_cast<double>(i) * line_height;
        text.push_back({panel.lines[source].get(), {24, y}});
        const auto block = std::ranges::find(panel.folds, source, &FoldBlock::first);
        if (block != panel.folds.end()) {
            if (block->collapsed) {
                const auto summary = std::format("  … {} lines", block->last - block->first);
                auto preview = make_line(summary, {{0, summary.size(), Syntax::Comment}});
                int width = 0;
                pango_layout_get_pixel_size(panel.lines[source].get(), &width, nullptr);
                text.push_back({preview.get(), {44.0 + width, y}});
                panel.fold_labels.push_back(std::move(preview));
            }
        }
    }
    if (!panel.image)
        panel.bounds.size.y = header_height + panel.folded.source_lines.size() * line_height;
    panel.bounds.size.x += panel.mesh.buffer ? 0 : 32;
    if (!panel.image)
        renderer->upload(panel.mesh, text);
    if (!panel.header_mesh.buffer)
        renderer->upload(panel.header_mesh, {{panel.title.get(), {2, 0}}});
}

void Application::relayout()
{
    std::vector<GraphFile> files;
    for (const auto& panel : panels)
        files.push_back({panel.path, panel.source, panel.bounds.size});
    dependencies = file_dependencies(files);
    graph = folder_layout(files, dependencies);
    apply_graph_layout();
}

void Application::resize_in_place(const SourcePanel& panel, double old_height)
{
    const double delta = panel.bounds.size.y - old_height;
    const double left = panel.bounds.origin.x, right = left + panel.bounds.size.x;
    for (auto& other : panels)
        if (&other != &panel && other.bounds.origin.y >= panel.bounds.origin.y + old_height &&
            other.bounds.origin.x < right && other.bounds.origin.x + other.bounds.size.x > left)
            other.bounds.origin.y += delta;
    refresh_project_geometry(false);
}

void Application::place_in_folder(SourcePanel& panel, const std::set<std::string>& unplaced)
{
    const auto folder = std::filesystem::path(panel.path).parent_path();
    std::map<double, double> columns;
    for (const auto& other : panels) {
        if (&other == &panel || unplaced.contains(other.path) ||
            std::filesystem::path(other.path).parent_path() != folder)
            continue;
        auto& bottom = columns[other.bounds.origin.x];
        bottom = std::max(bottom, other.bounds.origin.y + other.bounds.size.y);
    }
    const auto shortest = std::ranges::min_element(
        columns, [](const auto& a, const auto& b) { return a.second < b.second; });
    panel.bounds.origin = columns.empty() ? Vec2{content_size.x + 480, 480}
                                          : Vec2{shortest->first, shortest->second + 96};
    for (bool moved = true; moved;) {
        moved = false;
        for (const auto& other : panels)
            if (&other != &panel && !unplaced.contains(other.path) &&
                panel.bounds.intersects(other.bounds)) {
                panel.bounds.origin.y = other.bounds.origin.y + other.bounds.size.y + 96;
                moved = true;
            }
    }
}

void Application::apply_graph_layout()
{
    for (std::size_t i = 0; i < panels.size(); ++i)
        panels[i].bounds.origin = graph.files[i].origin;
    auto bounds = conversation_bounds();
    bounds.push_back({{0, 0}, graph.size});
    content_size = canvas_size(bounds);
    connections_dirty = true;
}

void Application::apply_styles(SourcePanel& panel)
{
    for (std::size_t line = 0; line < panel.lines.size(); ++line) {
        const AttributesHandle attributes{pango_attr_list_copy(panel.lexical[line].get())};
        auto add = [&](PangoAttribute* attribute, std::size_t begin, std::size_t end) {
            attribute->start_index = static_cast<guint>(begin);
            attribute->end_index = static_cast<guint>(end);
            pango_attr_list_change(attributes.get(), attribute);
        };
        for (const auto& token : panel.semantics) {
            if (token.line != static_cast<int>(line))
                continue;
            const auto color = colors[static_cast<std::size_t>(semantic_syntax(token.type))];
            add(pango_attr_foreground_new(((color >> 16) & 255) * 257, ((color >> 8) & 255) * 257,
                                          (color & 255) * 257),
                token.begin, token.begin + token.length);
            if (token.modifiers & deprecated_modifier)
                add(pango_attr_strikethrough_new(TRUE), token.begin, token.begin + token.length);
        }
        apply_diagnostics(panel, line, attributes.get());
        pango_layout_set_attributes(panel.lines[line].get(), attributes.get());
    }
    update_mesh(panel);
}

SourceHit Application::hit_test(Vec2 screen)
{
    const auto world = camera.screen_to_world(screen, viewport());
    if (chat_at(screen))
        return {};
    for (std::size_t i = 0; i < panels.size(); ++i) {
        const auto& panel = panels[i];
        const double x = world.x - (panel.bounds.origin.x + 24);
        const double y = world.y - (panel.bounds.origin.y + header_height);
        if (x < 0 || x >= panel.bounds.size.x || y < 0)
            continue;
        const int row = static_cast<int>(y / line_height);
        if (row >= std::ssize(panel.folded.source_lines))
            continue;
        const int line = panel.folded.source_lines[row];
        if (const auto byte =
                source_line_hit(panel.lines[line].get(), panel.text[line], x, font_height))
            return {static_cast<int>(i), line, *byte};
    }
    return {};
}

int Application::panel_at(Vec2 screen)
{
    const auto world = camera.screen_to_world(screen, viewport());
    if (chat_at(screen))
        return -1;
    for (std::size_t i = 0; i < panels.size(); ++i)
        if (Viewport{panels[i].bounds.origin.x, panels[i].bounds.origin.y, panels[i].bounds.size.x,
                     panels[i].bounds.size.y}
                .contains(world))
            return static_cast<int>(i);
    return -1;
}

int Application::query_at_pointer(const std::string& method)
{
    const auto hit = hit_test(mouse_position);
    if (std::getenv("SOURCE_CANVAS_TRACE_LSP"))
        std::cerr << method << " at " << mouse_position.x << ',' << mouse_position.y << " -> panel "
                  << hit.panel << " line " << hit.line << " byte " << hit.byte << '\n';
    if (hit.panel < 0 || !panels[hit.panel].cpp)
        return 0;
    const auto& panel = panels[hit.panel];
    auto* server = server_for(panel);
    const auto& failure = language_failure;
    if (!language_ready || !server) {
        show_popup(failure.empty() ? "Language features are starting…"
                                   : "Language features unavailable: " + failure,
                   mouse_position);
        return 0;
    }
    if (opened_documents.insert(panel.uri).second)
        server->open({panel.uri, panel.source});
    lsp::Json params = {
        {"textDocument", {{"uri", panel.uri}}},
        {"position",
         {{"line", hit.line}, {"character", lsp::byte_to_utf16(panel.text[hit.line], hit.byte)}}}};
    if (method == "textDocument/references")
        params["context"] = {{"includeDeclaration", false}};
    if (method == "textDocument/hover") {
        hover_origin = mouse_position;
        hover_prefix.clear();
        for (const auto& diagnostic : panel.diagnostics) {
            const auto& range = diagnostic.at("range");
            if (range["start"]["line"].get<int>() <= hit.line &&
                range["end"]["line"].get<int>() >= hit.line) {
                if (!hover_prefix.empty())
                    hover_prefix += "\n\n";
                hover_prefix += diagnostic.value("message", "");
            }
        }
    }
    return server->request(method, panel.uri, std::move(params));
}

bool Application::jump_to(const lsp::Json& location)
{
    const auto uri = location.value("uri", location.value("targetUri", ""));
    auto range = location.contains("targetSelectionRange")
                     ? location["targetSelectionRange"]
                     : location.value("range", lsp::Json::object());
    if (uri.empty() || !range.contains("start"))
        return false;
    auto found = std::ranges::find(panels, uri, &SourcePanel::uri);
    if (found == panels.end()) {
        const auto path = lsp::uri_path(uri);
        if (path.empty())
            return false;
        std::ifstream input(path);
        if (!input)
            return false;
        const std::string text{std::istreambuf_iterator<char>(input), {}};
        auto panel = make_panel(path, text);
        update_mesh(panel);
        panel.bounds.origin = {content_size.x + 96, 0};
        content_size.x = panel.bounds.origin.x + panel.bounds.size.x;
        content_size.y = std::max(content_size.y, panel.bounds.size.y);
        auto* server = server_for(panel);
        if (server && opened_documents.insert(panel.uri).second)
            server->open({panel.uri, text});
        panels.push_back(std::move(panel));
        found = std::prev(panels.end());
    }
    const int line = range["start"].value("line", 0);
    if (line < 0 || line >= std::ssize(found->lines))
        return false;
    remember_view();
    const double old_height = found->bounds.size.y;
    if (reveal_line(line, found->folds)) {
        update_mesh(*found);
        resize_in_place(*found, old_height);
    }
    const auto byte = lsp::utf16_to_byte(found->text[line], range["start"].value("character", 0));
    PangoRectangle position{};
    pango_layout_index_to_pos(found->lines[line].get(), static_cast<int>(byte), &position);
    camera.zoom = std::max(1.0, camera.zoom);
    camera.center = {found->bounds.origin.x + 24 + static_cast<double>(position.x) / PANGO_SCALE,
                     found->bounds.origin.y + header_height +
                         found->folded.display_lines[line] * line_height + line_height / 2.0};
    zoom_animation.cancel(camera);
    finish_view();
    focused_line = SourceHit{static_cast<int>(found - panels.begin()), line, byte};
    hide_popup();
    redraw = true;
    return true;
}

void Application::toggle_fold(int index, int source)
{
    auto& panel = panels[index];
    const auto found = std::ranges::find(panel.folds, source, &FoldBlock::first);
    if (found == panel.folds.end())
        return;
    found->collapsed = !found->collapsed;
    const double old_height = panel.bounds.size.y;
    update_mesh(panel);
    resize_in_place(panel, old_height);
    connections_dirty = true;
    redraw = true;
}

void Application::apply_diagnostics(const SourcePanel& panel, std::size_t line,
                                    PangoAttrList* attributes)
{
    auto add = [&](PangoAttribute* attribute, std::size_t begin, std::size_t end) {
        attribute->start_index = static_cast<guint>(begin);
        attribute->end_index = static_cast<guint>(end);
        pango_attr_list_change(attributes, attribute);
    };
    for (const auto& diagnostic : panel.diagnostics) {
        const auto& range = diagnostic.at("range");
        const int start_line = range.at("start").at("line");
        const int end_line = range.at("end").at("line");
        const int severity = diagnostic.value("severity", 1);
        if (severity > 2 || static_cast<int>(line) < start_line ||
            static_cast<int>(line) > end_line)
            continue;
        if (static_cast<int>(line) == end_line && end_line > start_line &&
            range["end"].value("character", 0) == 0)
            continue;
        const auto begin = static_cast<int>(line) == start_line
                               ? lsp::utf16_to_byte(panel.text[line], range["start"]["character"])
                               : 0;
        auto end = static_cast<int>(line) == end_line
                       ? lsp::utf16_to_byte(panel.text[line], range["end"]["character"])
                       : panel.text[line].size();
        if (end == begin && begin < panel.text[line].size())
            end = static_cast<std::size_t>(g_utf8_next_char(panel.text[line].c_str() + begin) -
                                           panel.text[line].c_str());
        if (end <= begin)
            continue;
        add(pango_attr_underline_new(PANGO_UNDERLINE_ERROR), begin, end);
        add(pango_attr_underline_color_new(severity == 1 ? 0xd000 : 0xa000,
                                           severity == 1 ? 0x2200 : 0x6500, 0x2200),
            begin, end);
    }
}

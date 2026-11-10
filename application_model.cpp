#include "application.h"

SourcePanel Application::make_panel(const std::string& path, const std::string& text)
{
    SourcePanel panel;
    panel.path = path;
    panel.image = project_image(path);
    panel.source = panel.image ? std::string{} : text;
    panel.uri = lsp::file_uri((std::filesystem::path(workspace_root) / path).string());
    const auto extension = std::filesystem::path(path).extension().string();
    panel.cpp = extension == ".cpp" || extension == ".cc" || extension == ".cxx" ||
                extension == ".c" || extension == ".h" || extension == ".hpp";
    const std::string icon_name = file_icon(path);
    load_icon(icon_name);
    panel.icon = icons[icon_name];
    panel.gpu_icon = gpu_icons[icon_name];
    panel.title = make_line(path, {{0, path.size(), Syntax::Directive}});
    int text_width = 0;
    pango_layout_get_pixel_size(panel.title, &text_width, nullptr);
    text_width += 32;
    panel.cmake =
        std::filesystem::path(path).filename() == "CMakeLists.txt" || extension == ".cmake";
    if (panel.image) {
        load_image_panel(panel, text, text_width);
        return panel;
    }
    HighlightState highlight_state;
    std::vector<int> brace_stack;
    std::istringstream source(text);
    std::string line;
    while (std::getline(source, line)) {
        const auto tokens = highlight(line, highlight_state, panel.cmake);
        auto* layout = make_line(line, tokens);
        discover_braces(panel, line, tokens, brace_stack);
        int line_width = 0;
        pango_layout_get_pixel_size(layout, &line_width, nullptr);
        text_width = std::max(text_width, line_width);
        panel.lines.push_back(layout);
        panel.text.push_back(line);
        panel.lexical.push_back(pango_attr_list_ref(pango_layout_get_attributes(layout)));
    }
    panel.bounds.size = {static_cast<double>(text_width + 4),
                         header_height + static_cast<double>(panel.lines.size()) * line_height};
    for (auto& block : panel.folds) {
        if (block.last - block.first < 60)
            continue;
        const bool nested =
            std::any_of(panel.folds.begin(), panel.folds.end(), [&](const auto& other) {
                return other.first < block.first && other.last >= block.last;
            });
        block.collapsed = !nested;
    }
    return panel;
}

void Application::update_mesh(SourcePanel& panel)
{
    connections_dirty = true;
    panel.folded = fold_view(static_cast<int>(panel.lines.size()), panel.folds);
    for (auto* label : panel.fold_labels)
        g_object_unref(label);
    panel.fold_labels.clear();
    std::vector<PlacedText> text;
    text.reserve(panel.lines.size() + 1);
    for (std::size_t i = 0; i < panel.folded.source_lines.size(); ++i) {
        const int source = panel.folded.source_lines[i];
        const double y = header_height + static_cast<double>(i) * line_height;
        text.push_back({panel.lines[source], {24, y}});
        const auto block = std::find_if(panel.folds.begin(), panel.folds.end(),
                                        [&](const auto& block) { return block.first == source; });
        if (block != panel.folds.end()) {
            if (block->collapsed) {
                const auto summary = "  … " + std::to_string(block->last - block->first) + " lines";
                auto* preview = make_line(summary, {{0, summary.size(), Syntax::Comment}});
                int width = 0;
                pango_layout_get_pixel_size(panel.lines[source], &width, nullptr);
                panel.fold_labels.push_back(preview);
                text.push_back({preview, {44.0 + width, y}});
            }
        }
    }
    if (!panel.image)
        panel.bounds.size.y = header_height + panel.folded.source_lines.size() * line_height;
    panel.bounds.size.x += panel.mesh.buffer ? 0 : 32;
    std::vector<PlacedImage> images;
    if (panel.icon)
        images.push_back({panel.gpu_icon, {{0, 0}, {24, 24}}});
    if (!panel.image)
        renderer->upload(panel.mesh, text);
    if (!panel.header_mesh.buffer)
        renderer->upload(panel.header_mesh, {{panel.title, {32, 0}}}, images);
}

void Application::relayout()
{
    std::vector<GraphFile> files;
    for (const auto& panel : panels)
        files.push_back({panel.path, panel.source, panel.bounds.size});
    dependencies = file_dependencies(files);
    try {
        graph = graph_layout(files, dependencies, workspace_build);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        graph.files = arrange_files(file_sizes);
        graph.routes.clear();
        graph.size = canvas_size(graph.files);
    }
    for (std::size_t i = 0; i < panels.size(); ++i)
        panels[i].bounds.origin = graph.files[i].origin;
    content_size = graph.size;
    for (const auto& card : chat_windows) {
        if (!card->visible)
            continue;
        content_size.x = std::max(content_size.x, card->bounds.origin.x + card->bounds.size.x);
        content_size.y = std::max(content_size.y, card->bounds.origin.y + card->bounds.size.y);
    }
    if (chat && chat->visible) {
        content_size.x = std::max(content_size.x, chat->bounds.origin.x + chat->bounds.size.x);
        content_size.y = std::max(content_size.y, chat->bounds.origin.y + chat->bounds.size.y);
    }
    if (intent && intent->placed && intent->visible) {
        content_size.x = std::max(content_size.x, intent->bounds.origin.x + intent->bounds.size.x);
        content_size.y = std::max(content_size.y, intent->bounds.origin.y + intent->bounds.size.y);
    }
    connections_dirty = true;
}

void Application::apply_styles(SourcePanel& panel)
{
    for (std::size_t line = 0; line < panel.lines.size(); ++line) {
        auto* attributes = pango_attr_list_copy(panel.lexical[line]);
        auto add = [&](PangoAttribute* attribute, std::size_t begin, std::size_t end) {
            attribute->start_index = static_cast<guint>(begin);
            attribute->end_index = static_cast<guint>(end);
            pango_attr_list_change(attributes, attribute);
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
        apply_diagnostics(panel, line, attributes);
        pango_layout_set_attributes(panel.lines[line], attributes);
        pango_attr_list_unref(attributes);
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
        const double x = world.x - panel.bounds.origin.x - 24;
        const double y = world.y - panel.bounds.origin.y - header_height;
        if (x < 0 || x >= panel.bounds.size.x || y < 0)
            continue;
        const int row = static_cast<int>(y / line_height);
        if (row >= static_cast<int>(panel.folded.source_lines.size()))
            continue;
        const int line = panel.folded.source_lines[row];
        int index = 0, trailing = 0;
        if (!pango_layout_xy_to_index(panel.lines[line], static_cast<int>(x * PANGO_SCALE),
                                      font_height * PANGO_SCALE / 2, &index, &trailing))
            continue;
        const auto& text = panel.text[line];
        if (index < 0 || index >= static_cast<int>(text.size()))
            continue;
        const auto byte = static_cast<std::size_t>(index);
        if (byte >= text.size() || std::isspace(static_cast<unsigned char>(text[byte])))
            continue;
        return {static_cast<int>(i), line, byte};
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

bool Application::definition_at_pointer()
{
    const auto hit = hit_test(mouse_position);
    if (hit.panel < 0)
        return false;
    const auto& panel = panels[hit.panel];
    for (const auto& token : panel.semantics)
        if (token.line == hit.line && hit.byte >= token.begin &&
            hit.byte < token.begin + token.length)
            return (token.modifiers & definition_modifier) != 0;
    return symbol_definition(panel.symbols, hit.line,
                             lsp::byte_to_utf16(panel.text[hit.line], hit.byte));
}

int Application::query_at_pointer(const std::string& method)
{
    const auto hit = hit_test(mouse_position);
    if (std::getenv("SOURCE_CANVAS_TRACE_LSP"))
        std::cerr << method << " at " << mouse_position.x << ',' << mouse_position.y << " -> panel "
                  << hit.panel << " line " << hit.line << " byte " << hit.byte << '\n';
    if (hit.panel < 0 || (!panels[hit.panel].cpp && !panels[hit.panel].cmake))
        return 0;
    const auto& panel = panels[hit.panel];
    auto* server = server_for(panel);
    const auto& failure = panel.cmake ? cmake_failure : language_failure;
    if (!(panel.cmake ? cmake_ready : language_ready) || !server) {
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
    auto found = std::find_if(panels.begin(), panels.end(),
                              [&](const auto& panel) { return panel.uri == uri; });
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
    if (line < 0 || line >= static_cast<int>(found->lines.size()))
        return false;
    remember_view();
    if (reveal_line(line, found->folds)) {
        update_mesh(*found);
        relayout();
        connections_dirty = true;
    }
    const auto byte = lsp::utf16_to_byte(found->text[line], range["start"].value("character", 0));
    PangoRectangle position{};
    pango_layout_index_to_pos(found->lines[line], static_cast<int>(byte), &position);
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
    const auto found = std::find_if(panel.folds.begin(), panel.folds.end(),
                                    [&](const auto& block) { return block.first == source; });
    if (found == panel.folds.end())
        return;
    found->collapsed = !found->collapsed;
    const auto old = panel.bounds.origin;
    update_mesh(panel);
    relayout();
    camera.center.x += panel.bounds.origin.x - old.x;
    camera.center.y += panel.bounds.origin.y - old.y;
    connections_dirty = true;
    redraw = true;
}

void Application::load_icon(const std::string& icon_name)
{
    if (!icons.count(icon_name)) {
        const auto icon_path = executable.parent_path() / "assets/vscode-icons" / icon_name;
        GError* error = nullptr;
        icons[icon_name] = rsvg_handle_new_from_file(icon_path.c_str(), &error);
        if (error) {
            std::cerr << error->message << '\n';
            g_error_free(error);
        }
        if (icons[icon_name]) {
            auto* image = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 192, 192);
            auto* image_cr = cairo_create(image);
            const RsvgRectangle bounds{0, 0, 192, 192};
            rsvg_handle_render_document(icons[icon_name], image_cr, &bounds, nullptr);
            gpu_icons[icon_name] = renderer->image(image);
            cairo_destroy(image_cr);
            cairo_surface_destroy(image);
        }
    }
}

void Application::discover_braces(SourcePanel& panel, const std::string& line,
                                  const std::vector<Token>& tokens, std::vector<int>& brace_stack)
{
    for (const auto& token : tokens) {
        if (token.syntax != Syntax::Plain)
            continue;
        for (std::size_t offset = token.begin; offset < token.begin + token.length; ++offset) {
            if (line[offset] == '{')
                brace_stack.push_back(static_cast<int>(panel.lines.size()));
            if (line[offset] == '}' && !brace_stack.empty()) {
                const int first = brace_stack.back();
                brace_stack.pop_back();
                const int last = static_cast<int>(panel.lines.size());
                if (last > first + 1)
                    panel.folds.push_back({first, last, false});
            }
        }
    }
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

#include "application.h"

PangoLayout* Application::make_line(const std::string& line, const std::vector<Token>& tokens)
{
    PangoLayout* layout = pango_layout_copy(base_layout);
    pango_layout_set_text(layout, line.c_str(), static_cast<int>(line.size()));
    PangoAttrList* attributes = pango_attr_list_new();
    pango_attr_list_insert(attributes, pango_attr_font_features_new("liga=1,clig=1,calt=1"));
    for (const auto& token : tokens) {
        auto add = [&](PangoAttribute* attribute) {
            attribute->start_index = static_cast<guint>(token.begin);
            attribute->end_index = static_cast<guint>(token.begin + token.length);
            pango_attr_list_insert(attributes, attribute);
        };
        const auto color = colors[static_cast<std::size_t>(token.syntax)];
        add(pango_attr_foreground_new(((color >> 16) & 255) * 257, ((color >> 8) & 255) * 257,
                                      (color & 255) * 257));
        if (token.syntax == Syntax::Keyword || token.syntax == Syntax::Directive) {
            add(pango_attr_weight_new(PANGO_WEIGHT_BOLD));
        } else if (token.syntax == Syntax::Comment) {
            add(pango_attr_style_new(PANGO_STYLE_ITALIC));
        }
    }
    pango_layout_set_attributes(layout, attributes);
    pango_attr_list_unref(attributes);
    return layout;
}

Viewport Application::viewport()
{
    return {0, 0, static_cast<double>(std::max(1, width)),
            static_cast<double>(std::max(1, height))};
}

void Application::fit_width()
{
    const auto view = viewport();
    camera.zoom = std::clamp(view.width / std::max(1.0, content_size.x), Camera::min_zoom, 1.0);
    camera.home(view);
    zoom_animation.cancel(camera);
}

void Application::draw()
{
    const auto view = viewport();
    if (initial_view) {
        camera.fit(content_size, view);
        zoom_animation.cancel(camera);
        initial_view = false;
    }
    renderer->begin(width, height);
    draw_connections(view);
    draw_files(view);

    draw_headers(view);
    draw_fold_toggle();
    draw_conversation();
    draw_popup();
    renderer->present();
    update_cursor();
}

void Application::draw_connections(const Viewport& view)
{
    const auto screen_center = view.center();
    if (show_connections) {
        if (connections_dirty)
            rebuild_connections();
        renderer->draw(connection_mesh,
                       {screen_center.x - camera.center.x * camera.zoom,
                        screen_center.y - camera.center.y * camera.zoom},
                       camera.zoom);
    }
}

void Application::draw_files(const Viewport& view)
{
    const auto screen_center = view.center();
    const CanvasRect visible{camera.screen_to_world({view.x, view.y}, view),
                             {view.width / camera.zoom, view.height / camera.zoom}};
    for (std::size_t i = 0; i < panels.size(); ++i) {
        const auto& panel = panels[i];
        if (!panel.bounds.intersects(visible))
            continue;
        if (language_server && panel.cpp && camera.zoom >= 0.35 &&
            opened_documents.insert(panel.uri).second)
            language_server->open({panel.uri, panel.source});
        const Vec2 origin{screen_center.x + (panel.bounds.origin.x - camera.center.x) * camera.zoom,
                          screen_center.y +
                              (panel.bounds.origin.y - camera.center.y) * camera.zoom};
        if (focused_line && focused_line->panel == static_cast<int>(i)) {
            renderer->rectangle(
                {{origin.x,
                  origin.y + (header_height +
                              panel.folded.display_lines[focused_line->line] * line_height) *
                                 camera.zoom},
                 {panel.bounds.size.x * camera.zoom, line_height * camera.zoom}},
                {0.92f, 0.95f, 1, 1});
        }
        // One cached GPU mesh per file; navigation only updates uniforms.
        renderer->draw(panel.mesh, origin, camera.zoom);
    }
}

void Application::draw_headers(const Viewport& view)
{
    header_hits.assign(panels.size(), {});
    for (std::size_t i = 0; i < panels.size(); ++i) {
        const auto& panel = panels[i];
        const auto point = camera.world_to_screen(panel.bounds.origin, view);
        const double scale = std::max(0.7, camera.zoom);
        int title_width = 0;
        pango_layout_get_pixel_size(panel.title, &title_width, nullptr);
        CanvasRect bounds{{point.x, point.y}, {(title_width + 34) * scale, 27 * scale}};
        if (bounds.origin.y < -bounds.size.y || bounds.origin.y > height ||
            bounds.origin.x > width || bounds.origin.x + bounds.size.x < 0)
            continue;
        bounds.origin.x = std::clamp(bounds.origin.x, 0.0, std::max(0.0, width - bounds.size.x));
        for (std::size_t j = 0; j < i; ++j)
            if (header_hits[j].size.x > 0 && bounds.intersects(header_hits[j]))
                bounds.origin.y = header_hits[j].origin.y + header_hits[j].size.y + 2;
        header_hits[i] = bounds;
        renderer->rectangle(bounds, {1, 1, 1, 0.94f});
        renderer->draw(panel.header_mesh, bounds.origin, scale);
    }
}

CanvasRect Application::fold_toggle_bounds(const SourcePanel& panel, int line)
{
    int text_width = 0;
    pango_layout_get_pixel_size(panel.lines[line], &text_width, nullptr);
    const int row = panel.folded.display_lines[line];
    return {{panel.bounds.origin.x + 28 + text_width,
             panel.bounds.origin.y + header_height + row * line_height},
            {20, static_cast<double>(line_height)}};
}

SourceHit Application::fold_at(Vec2 screen)
{
    if (drag_button || !viewport().contains(screen))
        return {};
    if (!popup_text.empty() && Viewport{popup_bounds.origin.x, popup_bounds.origin.y,
                                        popup_bounds.size.x, popup_bounds.size.y}
                                   .contains(screen))
        return {};
    const auto world = camera.screen_to_world(screen, viewport());
    for (std::size_t i = 0; i < panels.size(); ++i) {
        const auto& panel = panels[i];
        const double y = world.y - panel.bounds.origin.y - header_height;
        if (y < 0)
            continue;
        const int row = static_cast<int>(y / line_height);
        if (row >= static_cast<int>(panel.folded.source_lines.size()))
            continue;
        const int line = panel.folded.source_lines[row];
        const auto block = std::find_if(panel.folds.begin(), panel.folds.end(),
                                        [&](const auto& fold) { return fold.first == line; });
        if (block == panel.folds.end())
            continue;
        const auto bounds = fold_toggle_bounds(panel, line);
        const auto first = panel.text[line].find_first_not_of(" \t");
        PangoRectangle glyph{};
        const int byte = first == std::string::npos ? 0 : static_cast<int>(first);
        pango_layout_index_to_pos(panel.lines[line], byte, &glyph);
        const double start =
            panel.bounds.origin.x + 24 + static_cast<double>(glyph.x) / PANGO_SCALE;
        if (world.x >= start && world.x <= bounds.origin.x + bounds.size.x)
            return {static_cast<int>(i), line, 0};
    }
    return {};
}

void Application::draw_fold_toggle()
{
    const auto hit = fold_at(mouse_position);
    if (hit.panel < 0)
        return;
    const auto& panel = panels[hit.panel];
    const auto block = std::find_if(panel.folds.begin(), panel.folds.end(),
                                    [&](const auto& fold) { return fold.first == hit.line; });
    const auto bounds = fold_toggle_bounds(panel, hit.line);
    const auto point = camera.world_to_screen(bounds.origin, viewport());
    renderer->draw(fold_toggle_meshes[block->collapsed ? 1 : 0], point, camera.zoom);
}

void Application::draw_popup()
{
    if (!popup_text.empty()) {
        const int layout_width = std::max(1, std::min(620, width - 24));
        if (cached_popup != popup_text || cached_popup_width != layout_width ||
            cached_popup_code_lines != popup_code_lines) {
            update_popup_mesh(layout_width);
        }
        int text_width = 0, text_height = 0;
        pango_layout_get_pixel_size(popup_layout, &text_width, &text_height);
        const double x =
            std::clamp(popup_origin.x + 16, 0.0, std::max(0.0, width - text_width - 24.0));
        const double visible_height =
            std::min(static_cast<double>(text_height), std::max(1.0, height - 48.0));
        const double y =
            std::clamp(popup_origin.y + 20, 0.0, std::max(0.0, height - visible_height - 24.0));
        popup_scroll = std::clamp(popup_scroll, 0.0, std::max(0.0, text_height - visible_height));
        popup_bounds = {{x, y}, {text_width + 24.0, visible_height + 24.0}};
        renderer->rectangle({{x + 3, y + 4}, popup_bounds.size}, {0.1f, 0.15f, 0.25f, 0.1f});
        renderer->rectangle(popup_bounds, {0.97f, 0.98f, 1, 1});
        renderer->clip({{x + 12, y + 12}, {static_cast<double>(text_width), visible_height}},
                       height);
        renderer->draw(popup_mesh, {x + 12, y + 12 - popup_scroll}, 1);
        renderer->clear_clip();
        if (text_height > visible_height) {
            const double thumb = std::max(12.0, visible_height * visible_height / text_height);
            const double offset =
                popup_scroll / (text_height - visible_height) * (visible_height - thumb);
            renderer->rectangle({{x + text_width + 17, y + 12 + offset}, {2, thumb}},
                                {0.5f, 0.54f, 0.6f, 0.5f});
        }
    }
}

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
    GpuPath path;
    path.color = {0.42f, 0.49f, 0.59f, 0.24f};
    path.width = 1;
    std::vector<Vec2> controls;
    if (index < graph.routes.size())
        controls = graph.routes[index];
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
        controls[1] = {std::max(start.x + 48, controls[1].x), start.y};
        controls[controls.size() - 2] = {std::min(end.x - 48, controls[controls.size() - 2].x),
                                         end.y};
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

void Application::rebuild_connections()
{

    std::vector<GpuPath> paths;
    for (std::size_t index = 0; index < dependencies.size(); ++index) {
        auto path = dependency_path(index);
        paths.push_back(std::move(path));
    }
    renderer->paths(connection_mesh, paths);
    connections_dirty = false;
}

void Application::add_popup_syntax(PangoAttrList* attributes, const std::string& text,
                                   std::size_t offset, HighlightState& state)
{
    auto* description = pango_font_description_from_string("JetBrains Mono");
    pango_font_description_set_absolute_size(description, 14 * PANGO_SCALE);
    auto* font = pango_attr_font_desc_new(description);
    pango_font_description_free(description);
    font->start_index = static_cast<guint>(offset);
    font->end_index = static_cast<guint>(offset + text.size());
    pango_attr_list_insert(attributes, font);
    for (const auto& token : highlight(text, state)) {
        const auto color = colors[static_cast<std::size_t>(token.syntax)];
        auto add = [&](PangoAttribute* attr) {
            attr->start_index = static_cast<guint>(offset + token.begin);
            attr->end_index = static_cast<guint>(offset + token.begin + token.length);
            pango_attr_list_insert(attributes, attr);
        };
        add(pango_attr_foreground_new(((color >> 16) & 255) * 257, ((color >> 8) & 255) * 257,
                                      (color & 255) * 257));
        if (token.syntax == Syntax::Keyword || token.syntax == Syntax::Directive)
            add(pango_attr_weight_new(PANGO_WEIGHT_BOLD));
        if (token.syntax == Syntax::Comment)
            add(pango_attr_style_new(PANGO_STYLE_ITALIC));
    }
}

void Application::style_document_spans(PangoAttrList* attributes,
                                       const std::vector<PopupSpan>& spans)
{
    for (const auto& span : spans) {
        auto add = [&](PangoAttribute* attr) {
            attr->start_index = static_cast<guint>(span.begin);
            attr->end_index = static_cast<guint>(span.begin + span.length);
            pango_attr_list_insert(attributes, attr);
        };
        switch (span.style) {
        case PopupStyle::Heading:
            add(pango_attr_scale_new(1.25));
            add(pango_attr_weight_new(PANGO_WEIGHT_BOLD));
            break;
        case PopupStyle::Strong:
            add(pango_attr_weight_new(PANGO_WEIGHT_BOLD));
            break;
        case PopupStyle::Emphasis:
            add(pango_attr_style_new(PANGO_STYLE_ITALIC));
            break;
        case PopupStyle::Link:
            add(pango_attr_foreground_new(0x2400, 0x4d00, 0x8600));
            break;
        }
    }
}

void Application::configure_document(PangoLayout* layout, const PopupDocument& document,
                                     int layout_width, bool prose)
{
    auto* font = pango_font_description_from_string(prose ? "Noto Serif" : "JetBrains Mono");
    pango_font_description_set_absolute_size(font, (prose ? 17 : 14) * PANGO_SCALE);
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);
    pango_layout_set_single_paragraph_mode(layout, FALSE);
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_NONE);
    pango_layout_set_spacing(layout, (prose ? 5 : 2) * PANGO_SCALE);
    pango_layout_set_width(layout, layout_width * PANGO_SCALE);
    pango_layout_set_text(layout, document.text.c_str(), -1);
    auto* attributes = pango_attr_list_new();
    pango_attr_list_insert(attributes, pango_attr_font_features_new("liga=1,clig=1,calt=1"));
    style_document_spans(attributes, document.styles);
    shape_document_math(attributes, document.math, layout_width);
    HighlightState state;
    std::istringstream input(document.text);
    std::string line;
    std::size_t offset = 0, index = 0;
    while (std::getline(input, line)) {
        if (index < document.code_lines.size() && document.code_lines[index])
            add_popup_syntax(attributes, line, offset, state);
        else
            state = {};
        offset += line.size() + 1;
        ++index;
    }
    for (const auto& [begin, length] : document.inline_code) {
        HighlightState inline_state;
        add_popup_syntax(attributes, document.text.substr(begin, length), begin, inline_state);
    }
    pango_layout_set_attributes(layout, attributes);
    pango_attr_list_unref(attributes);
}

void Application::update_popup_mesh(int layout_width)
{
    PopupDocument document;
    document.text = popup_text;
    document.code_lines = popup_code_lines;
    document.styles = popup_styles;
    document.math = popup_math;
    document.inline_code = popup_inline_code;
    configure_document(popup_layout, document, layout_width, popup_prose);
    upload_document_mesh(popup_mesh, popup_layout, popup_math);
    cached_popup = popup_text;
    cached_popup_width = layout_width;
    cached_popup_code_lines = popup_code_lines;
}

void Application::update_document(DocumentView& view, const std::string& source, int width,
                                  bool prose)
{
    if (view.source == source && view.width == width && view.layout &&
        view.generation == document_generation)
        return;
    view.source = source;
    view.width = width;
    view.generation = document_generation;
    view.document = popup_document(source, prose);
    if (!view.layout)
        view.layout.reset(pango_layout_copy(base_layout));
    configure_document(view.layout.get(), view.document, width, prose);
    upload_document_mesh(view.mesh, view.layout.get(), view.document.math);
    int text_width = 0, height = 0;
    pango_layout_get_pixel_size(view.layout.get(), &text_width, &height);
    view.size = {static_cast<double>(text_width), static_cast<double>(height)};
}

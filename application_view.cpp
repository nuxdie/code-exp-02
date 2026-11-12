#include "application.h"

namespace {
// Prose is set as prose: a serif face, upright, with the comment marker left out. Text in
// backticks stays in the code face.
void set_as_prose(PangoAttrList* attributes, const std::string& line, const ProseLine& prose,
                  const char* code_family)
{
    auto add = [&](PangoAttribute* attribute, std::size_t begin, std::size_t end) {
        attribute->start_index = static_cast<guint>(begin);
        attribute->end_index = static_cast<guint>(end);
        pango_attr_list_change(attributes, attribute);
    };
    if (prose.body > prose.begin) {
        const PangoRectangle none{0, 0, 0, 0};
        add(pango_attr_shape_new(&none, &none), prose.begin, prose.body);
    }
    if (prose.body == line.size())
        return;
    add(pango_attr_family_new("Noto Serif"), prose.body, line.size());
    add(pango_attr_size_new_absolute(17 * PANGO_SCALE), prose.body, line.size());
    add(pango_attr_style_new(PANGO_STYLE_NORMAL), prose.body, line.size());
    if (prose.heading)
        add(pango_attr_weight_new(PANGO_WEIGHT_BOLD), prose.body, line.size());
    for (auto open = line.find('`', prose.body); open != std::string::npos;) {
        const auto close = line.find('`', open + 1);
        if (close == std::string::npos)
            break;
        add(pango_attr_family_new(code_family), open, close + 1);
        add(pango_attr_size_new_absolute(15 * PANGO_SCALE), open, close + 1);
        open = line.find('`', close + 1);
    }
}
} // namespace

PangoLayout* Application::make_line(const std::string& line, const std::vector<Token>& tokens,
                                    const ProseLine* prose)
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
    style_document_spans(attributes, web_links(line));
    if (prose)
        set_as_prose(attributes, line, *prose, monospace_family);
    pango_layout_set_attributes(layout, attributes);
    pango_attr_list_unref(attributes);
    return layout;
}

Viewport Application::viewport()
{
    return {0, 0, static_cast<double>(std::max(1, width)),
            static_cast<double>(std::max(1, height))};
}

void Application::draw()
{
    const auto view = viewport();
    if (workspace_load) {
        draw_workspace_preview(view);
        return;
    }
    if (initial_view) {
        camera.fit(content_size, view);
        zoom_animation.cancel(camera);
        initial_view = false;
    }
    renderer->begin(width, height);
    advance_agent_activity();
    draw_regions(view);
    draw_connections(view);
    draw_activity_highlights();
    draw_intent_margin(view);
    draw_files(view);

    draw_headers(view);
    draw_fold_toggle();
    draw_intent_note();
    draw_connection_markers();
    draw_agent_activity();
    draw_conversation();
    draw_popup();
    draw_search();
    renderer->present();
    update_cursor();
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
        auto* server = server_for(panel);
        if (server && camera.zoom >= 0.35 && opened_documents.insert(panel.uri).second)
            server->open({panel.uri, panel.source});
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
        draw_source_selection(panel, origin);
        // One cached GPU mesh per file; navigation only updates uniforms.
        const Vec2 mesh_origin{origin.x,
                               origin.y + (panel.image ? header_height * camera.zoom : 0)};
        renderer->draw(panel.mesh, mesh_origin, camera.zoom);
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
        const int layout_width = std::max(
            1, std::min(shortcuts_visible ? 820 : 620, width - (shortcuts_visible ? 48 : 24)));
        if (cached_popup != popup_text || cached_popup_width != layout_width ||
            cached_popup_code_lines != popup_code_lines) {
            update_popup_mesh(layout_width);
        }
        int text_width = 0, text_height = 0;
        pango_layout_get_pixel_size(popup_layout, &text_width, &text_height);
        auto last = popup_text.find_last_not_of(" \t\r\n");
        if (last != std::string::npos) {
            while (last && (static_cast<unsigned char>(popup_text[last]) & 0xc0) == 0x80)
                --last;
            PangoRectangle tail{};
            pango_layout_index_to_pos(popup_layout, static_cast<int>(last), &tail);
            text_height = std::min(text_height, (tail.y + tail.height) / PANGO_SCALE);
        }
        const double popup_x =
            shortcuts_visible ? (width - text_width - 24.0) / 2 : popup_origin.x + 16;
        const double x = std::clamp(popup_x, 0.0, std::max(0.0, width - text_width - 24.0));
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
        if (shortcuts_visible)
            draw_shortcut_keys({x + 12, y + 12 - popup_scroll});
        if (text_selection.surface == TextSurface::Popup && text_selection.range.text == popup_text)
            draw_selection_layout(popup_layout, {x + 12, y + 12 - popup_scroll}, 1,
                                  text_selection.range.selection());
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

void Application::add_popup_syntax(PangoAttrList* attributes, const std::string& text,
                                   std::size_t offset, HighlightState& state)
{
    auto* description = pango_font_description_from_string(monospace_family);
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
            add(pango_attr_underline_new(PANGO_UNDERLINE_SINGLE));
            break;
        }
    }
}

void Application::configure_document(PangoLayout* layout, const PopupDocument& document,
                                     int layout_width, bool prose)
{
    auto* font = pango_font_description_from_string(prose ? "Noto Serif" : monospace_family);
    pango_font_description_set_absolute_size(font, (prose ? 17 : 14) * PANGO_SCALE);
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);
    pango_layout_set_tabs(layout, nullptr);
    pango_layout_set_single_paragraph_mode(layout, FALSE);
    pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_NONE);
    pango_layout_set_spacing(layout, (prose ? 5 : 2) * PANGO_SCALE);
    pango_layout_set_width(layout, layout_width * PANGO_SCALE);
    pango_layout_set_text(layout, document.text.c_str(), -1);
    auto* attributes = pango_attr_list_new();
    pango_attr_list_insert(attributes, pango_attr_font_features_new("liga=1,clig=1,calt=1"));
    style_document_spans(attributes, document.styles);
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

namespace {
void compact_popup_layout(PangoLayout* layout, const PopupDocument& document, bool prose)
{
    auto* font = pango_font_description_copy(pango_layout_get_font_description(layout));
    pango_font_description_set_absolute_size(font, (prose ? 16 : 14) * PANGO_SCALE);
    pango_layout_set_font_description(layout, font);
    pango_font_description_free(font);
    pango_layout_set_spacing(layout, PANGO_SCALE);
    auto* attributes = pango_attr_list_copy(pango_layout_get_attributes(layout));
    for (const auto& span : document.styles) {
        if (span.style != PopupStyle::Heading)
            continue;
        auto* scale = pango_attr_scale_new(1.1);
        scale->start_index = static_cast<guint>(span.begin);
        scale->end_index = static_cast<guint>(span.begin + span.length);
        pango_attr_list_change(attributes, scale);
    }
    std::istringstream input(document.text);
    std::string line;
    std::size_t offset = 0, row = 0;
    bool previous_blank = true;
    while (std::getline(input, line)) {
        const bool blank = line.find_first_not_of(" \t\r") == std::string::npos;
        const bool code = row < document.code_lines.size() && document.code_lines[row];
        if (blank && !code && prose) {
            auto* height =
                pango_attr_line_height_new_absolute((previous_blank ? 1 : 7) * PANGO_SCALE);
            height->start_index = static_cast<guint>(offset);
            height->end_index = static_cast<guint>(offset + line.size() + 1);
            pango_attr_list_change(attributes, height);
        }
        previous_blank = blank;
        offset += line.size() + 1;
        ++row;
    }
    pango_layout_set_attributes(layout, attributes);
    pango_attr_list_unref(attributes);
}
} // namespace

void Application::update_popup_mesh(int layout_width)
{
    PopupDocument document;
    document.text = popup_text;
    document.code_lines = popup_code_lines;
    document.styles = popup_styles;
    document.inline_code = popup_inline_code;
    configure_document(popup_layout, document, layout_width, popup_prose);
    compact_popup_layout(popup_layout, document, popup_prose);
    if (shortcuts_visible)
        style_shortcuts(layout_width);
    renderer->upload(popup_mesh, {{popup_layout, {0, 0}}});
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
    renderer->upload(view.mesh, {{view.layout.get(), {0, 0}}});
    int text_width = 0, height = 0;
    pango_layout_get_pixel_size(view.layout.get(), &text_width, &height);
    view.size = {static_cast<double>(text_width), static_cast<double>(height)};
}

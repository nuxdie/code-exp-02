#include "application.h"

void Application::search_line(DocumentView& view, const std::string& text, int row_width, bool code,
                              bool cmake)
{
    if (view.source == text && view.width == row_width && view.layout &&
        view.generation == document_generation)
        return;
    view.source = text;
    view.width = row_width;
    view.generation = document_generation;
    HighlightState state;
    view.layout = make_line(text, code ? highlight(text, state, cmake) : std::vector<Token>{});
    pango_layout_set_width(view.layout.get(), std::max(1, row_width) * PANGO_SCALE);
    pango_layout_set_ellipsize(view.layout.get(), PANGO_ELLIPSIZE_END);
    pango_layout_set_height(view.layout.get(), -1);
    renderer->upload(view.mesh, {{view.layout.get(), {0, 0}}});
}
void Application::draw_search_match(SearchMatch& match, Vec2 origin, double row_width)
{
    const auto label =
        match.filename ? match.path : std::format("{}:{}", match.path, match.line + 1);
    search_line(match.label, label, static_cast<int>(row_width), false);
    search_line(match.snippet, match.preview, static_cast<int>(row_width), !match.filename,
                match.cmake);
    renderer->draw(match.label.mesh, origin, 1);
    origin.y += 24;
    if (!match.filename)
        draw_selection_layout(match.snippet.layout.get(), origin, 1,
                              {match.preview_byte, match.preview_byte + match.length});
    renderer->draw(match.snippet.mesh, origin, 1);
}
void Application::draw_search()
{
    if (!search.active)
        return;
    const double box_width = std::max(120.0, std::min(820.0, width - 32.0));
    const double max_height = std::max(210.0, std::min(640.0, height - 48.0));
    const auto max_rows =
        static_cast<std::size_t>(std::max(1.0, std::floor((max_height - 150) / 56)));
    const auto rows = std::clamp(search.matches.size(), std::size_t{1}, max_rows);
    const double box_height = 150 + rows * 56;
    const double x = (width - box_width) / 2, y = std::max(0.0, (height - max_height) / 3);
    search.bounds = {{x, y}, {box_width, box_height}};
    search.editor = {{x + 20, y + 56}, {box_width - 40, 44}};
    renderer->rectangle({{x + 4, y + 6}, search.bounds.size}, {0.1f, 0.15f, 0.25f, 0.15f});
    renderer->rectangle(search.bounds, {0.97f, 0.98f, 1, 1});
    search_line(search.title, "Search project", static_cast<int>(box_width - 40), false);
    renderer->draw(search.title.mesh, {x + 20, y + 20}, 1);
    renderer->rectangle(search.editor, {1, 1, 1, 1});
    search_line(search.input,
                search.query.text.empty() ? "File names and text…" : search.query.text,
                static_cast<int>(search.editor.size.x - 24), false);
    if (pango_layout_get_width(search.input.layout.get()) != -1) {
        pango_layout_set_width(search.input.layout.get(), -1);
        pango_layout_set_ellipsize(search.input.layout.get(), PANGO_ELLIPSIZE_NONE);
        renderer->upload(search.input.mesh, {{search.input.layout.get(), {0, 0}}});
    }
    PangoRectangle caret{};
    pango_layout_index_to_pos(search.input.layout.get(), static_cast<int>(search.query.cursor),
                              &caret);
    const auto caret_x = static_cast<double>(caret.x) / PANGO_SCALE;
    search.editor_scroll = std::clamp(search.editor_scroll,
                                      std::max(0.0, caret_x - search.editor.size.x + 26), caret_x);
    const Vec2 input_origin{search.editor.origin.x + 12 - search.editor_scroll,
                            search.editor.origin.y + 10};
    renderer->clip(search.editor, height);
    if (!search.query.text.empty())
        draw_selection_layout(search.input.layout.get(), input_origin, 1, search.query.selection());
    renderer->draw(search.input.mesh, input_origin, 1);
    renderer->rectangle(
        {{input_origin.x + static_cast<double>(caret.x) / PANGO_SCALE, input_origin.y},
         {1, static_cast<double>(caret.height) / PANGO_SCALE}},
        {0.2f, 0.24f, 0.3f, 1});
    renderer->clear_clip();
    if (search.selected < search.offset)
        search.offset = search.selected;
    if (search.selected >= search.offset + rows)
        search.offset = search.selected - rows + 1;
    search.hits.clear();
    for (auto i = search.offset; i < std::min(search.matches.size(), search.offset + rows); ++i) {
        const Vec2 origin{x + 20, y + 112 + (i - search.offset) * 56};
        const CanvasRect bounds{origin, {box_width - 40, 54}};
        if (i == search.selected)
            renderer->rectangle(bounds, {0.89f, 0.93f, 1, 1});
        draw_search_match(search.matches[i], {origin.x + 8, origin.y + 3}, box_width - 56);
        search.hits.emplace_back(bounds, i);
    }
    const auto count =
        search.more ? "First 200 matches" : std::format("{} matches", search.matches.size());
    const auto hint = search.query.text.empty() ? "Type to search · Esc closes"
                      : search.matches.empty()  ? "No matches · Esc closes"
                                               : count + " · ↑/↓ choose · Enter jumps · Esc closes";
    search_line(search.hint, hint, static_cast<int>(box_width - 40), false);
    renderer->draw(search.hint.mesh, {x + 20, y + box_height - 30}, 1);
}

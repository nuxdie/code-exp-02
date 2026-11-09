#include "application.h"

void Application::draw_selection_layout(PangoLayout* layout, Vec2 origin, double scale,
                                        std::pair<std::size_t, std::size_t> range)
{
    const auto [begin, end] = range;
    if (!layout || begin == end)
        return;
    auto* iterator = pango_layout_get_iter(layout);
    do {
        auto* line = pango_layout_iter_get_line_readonly(iterator);
        PangoRectangle logical{};
        pango_layout_iter_get_line_extents(iterator, nullptr, &logical);
        const auto y = origin.y + static_cast<double>(logical.y) / PANGO_SCALE * scale;
        const auto row_height = static_cast<double>(logical.height) / PANGO_SCALE * scale;
        if (y + row_height < 0 || y > height)
            continue;
        int* ranges = nullptr;
        int count = 0;
        pango_layout_line_get_x_ranges(line, static_cast<int>(begin), static_cast<int>(end),
                                       &ranges, &count);
        for (int i = 0; i < count; ++i)
            renderer->rectangle(
                {{origin.x + static_cast<double>(ranges[2 * i]) / PANGO_SCALE * scale,
                  origin.y + static_cast<double>(logical.y) / PANGO_SCALE * scale},
                 {static_cast<double>(ranges[2 * i + 1] - ranges[2 * i]) / PANGO_SCALE * scale,
                  static_cast<double>(logical.height) / PANGO_SCALE * scale}},
                {0.70f, 0.81f, 0.98f, 0.85f});
        g_free(ranges);
    } while (pango_layout_iter_next_line(iterator));
    pango_layout_iter_free(iterator);
}
void Application::draw_source_selection(const SourcePanel& panel, Vec2 origin)
{
    if (text_selection.surface != TextSurface::Source || text_selection.key != panel.path ||
        text_selection.range.text != panel.source)
        return;
    const auto [begin, end] = text_selection.range.selection();
    std::size_t offset = 0;
    std::size_t row = 0;
    for (std::size_t i = 0; i < panel.text.size(); ++i) {
        const auto length = panel.text[i].size();
        if (row < panel.folded.source_lines.size() &&
            panel.folded.source_lines[row] == static_cast<int>(i)) {
            const auto y = origin.y + (header_height + row * line_height) * camera.zoom;
            const bool visible = y + line_height * camera.zoom >= 0 && y <= height;
            if (visible && begin < offset + length && end > offset)
                draw_selection_layout(
                    panel.lines[i],
                    {origin.x + 24 * camera.zoom,
                     origin.y + (header_height + row * line_height) * camera.zoom},
                    camera.zoom,
                    {begin > offset ? begin - offset : 0, std::min(length, end - offset)});
            ++row;
        }
        offset += length + 1;
    }
}

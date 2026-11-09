#include "application.h"

void Application::draw_input_selection(Vec2 origin)
{
    const auto [begin, end] = chat->input.selection();
    if (begin == end)
        return;
    auto* iterator = pango_layout_get_iter(chat->composer.layout.get());
    do {
        auto* line = pango_layout_iter_get_line_readonly(iterator);
        PangoRectangle logical{};
        pango_layout_iter_get_line_extents(iterator, nullptr, &logical);
        int* ranges = nullptr;
        int count = 0;
        pango_layout_line_get_x_ranges(line, static_cast<int>(begin), static_cast<int>(end),
                                       &ranges, &count);
        for (int i = 0; i < count; ++i)
            renderer->rectangle(
                {{origin.x + ranges[i * 2] / static_cast<double>(PANGO_SCALE) * camera.zoom,
                  origin.y + logical.y / static_cast<double>(PANGO_SCALE) * camera.zoom},
                 {(ranges[i * 2 + 1] - ranges[i * 2]) / static_cast<double>(PANGO_SCALE) *
                      camera.zoom,
                  logical.height / static_cast<double>(PANGO_SCALE) * camera.zoom}},
                {0.78f, 0.86f, 0.98f, 0.8f});
        g_free(ranges);
    } while (pango_layout_iter_next_line(iterator));
    pango_layout_iter_free(iterator);
}
void Application::move_chat_caret(int direction, bool extend)
{
    if (!chat->composer.layout)
        return;
    PangoRectangle cursor{};
    pango_layout_index_to_pos(chat->composer.layout.get(), static_cast<int>(chat->input.cursor),
                              &cursor);
    int index = 0, trailing = 0;
    pango_layout_xy_to_index(chat->composer.layout.get(), cursor.x,
                             cursor.y + cursor.height * direction + cursor.height / 2, &index,
                             &trailing);
    const auto byte =
        std::min(chat->input.text.size(), static_cast<std::size_t>(std::max(0, index)));
    const char* text = chat->input.text.c_str();
    chat->input.place(
        static_cast<std::size_t>(g_utf8_offset_to_pointer(text + byte, trailing) - text), extend);
}

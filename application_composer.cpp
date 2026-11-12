#include "application.h"

void Application::draw_input_selection(Vec2 origin)
{
    draw_selection_layout(chat->composer.layout.get(), origin, camera.zoom, chat->input.selection(),
                          {0.78f, 0.86f, 0.98f, 0.8f});
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

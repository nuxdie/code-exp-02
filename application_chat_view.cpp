#include "application.h"

void Application::draw_chat_card()
{
    if (!chat || !chat->visible)
        return;
    if (chat->composing)
        prepare_chat_prompt();
    const auto origin = camera.world_to_screen(chat->bounds.origin, viewport());
    const CanvasRect bounds{origin,
                            {chat->bounds.size.x * camera.zoom, chat->bounds.size.y * camera.zoom}};
    if (!bounds.intersects({{0, 0}, {static_cast<double>(width), static_cast<double>(height)}}))
        return;
    renderer->rectangle({{origin.x + 3, origin.y + 4}, bounds.size}, {0.1f, 0.15f, 0.2f, 0.08f});
    renderer->rectangle(bounds, {0.995f, 0.993f, 0.985f, 1});
    chat->hits.clear();
    if (chat->composing) {
        const double preview_height = chat->editor.origin.y - chat->bounds.origin.y;
        if (preview_height > 0) {
            renderer->rectangle({origin, {bounds.size.x, preview_height * camera.zoom}},
                                {0.92f, 0.95f, 0.98f, 1});
            const auto point = camera.world_to_screen(
                {chat->bounds.origin.x + 12, chat->bounds.origin.y + 12}, viewport());
            renderer->clip(
                {point,
                 {(chat->bounds.size.x - 24) * camera.zoom, (preview_height - 24) * camera.zoom}},
                height);
            renderer->draw(chat->context_preview.mesh, point, camera.zoom);
            renderer->clear_clip();
            chat->hits.push_back({ChatHit::Kind::Text,
                                  {{chat->bounds.origin.x + 12, chat->bounds.origin.y + 12},
                                   {chat->bounds.size.x - 24, preview_height - 24}},
                                  0,
                                  chat->context_preview.layout.get()});
        }
        draw_chat_controls();
        if (!chat->conversation.error.empty()) {
            const Vec2 point{chat->editor.origin.x + 12,
                             chat->editor.origin.y + chat->editor.size.y + 12};
            renderer->draw(chat->status.mesh, camera.world_to_screen(point, viewport()),
                           camera.zoom);
        }
        return;
    }
    chat->hits.push_back({ChatHit::Kind::Title, {chat->bounds.origin, {chat->bounds.size.x, 48}}});
    chat->body = {{chat->bounds.origin.x + 24, chat->bounds.origin.y + 64},
                  {chat->bounds.size.x - 48, chat->bounds.size.y - 196}};
    chat->editor = {{chat->bounds.origin.x + 20, chat->bounds.origin.y + chat->bounds.size.y - 112},
                    {chat->bounds.size.x - 40, 90}};
    if (!chat->conversation.error.empty()) {
        auto& error = chat->messages["__error"];
        update_document(error, chat->conversation.error, static_cast<int>(chat->body.size.x),
                        false);
        const double space = std::min(90.0, error.size.y) + 12;
        chat->body.origin.y += space;
        chat->body.size.y -= space;
    }
    draw_chat_chrome();
    prepare_chat_messages();
    if (chat->scrolling)
        drag_chat_scroll(camera.screen_to_world(mouse_position, viewport()));
    draw_chat_messages();
    draw_chat_controls();
}

void Application::draw_chat_chrome()
{
    const auto& model = chat->conversation;
    if (!model.error.empty()) {
        auto& error = chat->messages["__error"];
        update_document(error, model.error, static_cast<int>(chat->body.size.x), false);
        const auto point = camera.world_to_screen(
            {chat->bounds.origin.x + 24, chat->bounds.origin.y + 64}, viewport());
        renderer->clip({point, {chat->body.size.x * camera.zoom, 90 * camera.zoom}}, height);
        renderer->draw(error.mesh, point, camera.zoom);
        renderer->clear_clip();
        chat->hits.push_back({ChatHit::Kind::Text,
                              {{chat->bounds.origin.x + 24, chat->bounds.origin.y + 64},
                               {chat->body.size.x, std::min(90.0, error.size.y)}},
                              0,
                              error.layout.get()});
    }
}

void Application::draw_chat_header(std::vector<CanvasRect>& occupied)
{
    chat->header_bounds = {};
    if (chat->visible ? chat->composing : !kept_chat(*chat))
        return;
    draw_window_title(chat->header, chat->conversation.title, chat->bounds, chat->header_bounds,
                      occupied);
    const auto bounds = chat->header_bounds;
    const double scale = CanvasWindowManager::title_scale(camera.zoom);
    if (bounds.size.x == 0)
        return;
    if (!chat_icon_mesh.buffer) {
        const std::array<float, 4> blue{0.12f, 0.38f, 0.72f, 1};
        renderer->paths(chat_icon_mesh,
                        {{{{3, 3}, {21, 3}, {21, 17}, {11, 17}, {5, 22}, {5, 17}, {3, 17}, {3, 3}},
                          blue,
                          2,
                          false},
                         {{{7, 8}, {17, 8}}, blue, 2, false},
                         {{{7, 12}, {14, 12}}, blue, 2, false}});
    }
    renderer->draw(chat_icon_mesh, bounds.origin, scale);
}

void Application::draw_chat_messages()
{
    const auto screen = camera.world_to_screen(chat->body.origin, viewport());
    renderer->clip({screen, {chat->body.size.x * camera.zoom, chat->body.size.y * camera.zoom}},
                   height);
    double y = chat->body.origin.y - chat->scroll;
    for (std::size_t i = 0; i < chat->conversation.entries.size(); ++i) {
        const auto& entry = chat->conversation.entries[i];
        if (entry.activity)
            continue;
        const double entry_height = chat_entry_height(i);
        const auto bounds = chat->message_text_bounds(i, y);
        const auto found = chat->messages.find(entry.id);
        if (bounds.intersects(chat->body) && found != chat->messages.end() &&
            found->second.layout) {
            const auto bubble =
                camera.world_to_screen({bounds.origin.x - 12, bounds.origin.y - 10}, viewport());
            renderer->rectangle(
                {bubble, {(bounds.size.x + 24) * camera.zoom, (bounds.size.y + 20) * camera.zoom}},
                entry.role == "You" ? std::array<float, 4>{0.85f, 0.91f, 0.99f, 1}
                                    : std::array<float, 4>{0.94f, 0.95f, 0.96f, 1});
            if (text_selection.surface == TextSurface::Chat && text_selection.chat == chat.get() &&
                text_selection.key == entry.id &&
                text_selection.range.text == found->second.document.text)
                draw_selection_layout(found->second.layout.get(),
                                      camera.world_to_screen(bounds.origin, viewport()),
                                      camera.zoom, text_selection.range.selection());
            renderer->draw(found->second.mesh, camera.world_to_screen(bounds.origin, viewport()),
                           camera.zoom);
            chat->hits.push_back({ChatHit::Kind::Message, bounds, i});
        }
        y += entry_height;
    }
    draw_chat_working(y);
    renderer->clear_clip();
    draw_chat_scrollbar();
}

void Application::draw_chat_working(double y)
{
    const CanvasRect bounds{{chat->body.origin.x + 12, y}, {48, chat->working_height()}};
    if (bounds.size.y == 0 || !bounds.intersects(chat->body))
        return;
    const auto now = Clock::now();
    const double phase = std::chrono::duration<double>(now.time_since_epoch()).count() * 5;
    auto& dot = chat->messages["__working_dot"];
    update_document(dot, "•", 16, false);
    for (int i = 0; i < 3; ++i) {
        const double lift = 3 * std::max(0.0, std::sin(phase - i * 0.8));
        const Vec2 point{bounds.origin.x + i * 12, y + 6 - lift};
        renderer->draw(dot.mesh, camera.world_to_screen(point, viewport()), camera.zoom);
    }
    chat_animation_due = std::min(chat_animation_due, now + std::chrono::milliseconds(33));
}

void Application::draw_chat_controls()
{
    const auto origin = camera.world_to_screen(chat->editor.origin, viewport());
    renderer->rectangle(
        {origin, {chat->editor.size.x * camera.zoom, chat->editor.size.y * camera.zoom}},
        chat->focused ? std::array<float, 4>{1, 1, 1, 1}
                      : std::array<float, 4>{0.97f, 0.97f, 0.96f, 1});
    const bool placeholder = chat->input.text.empty();
    update_document(chat->composer, placeholder ? "Message…" : chat->input.text,
                    static_cast<int>(chat->editor.size.x - 24), false);
    PangoRectangle cursor{};
    pango_layout_index_to_pos(chat->composer.layout.get(),
                              placeholder ? 0 : static_cast<int>(chat->input.cursor), &cursor);
    const double cursor_y = static_cast<double>(cursor.y) / PANGO_SCALE;
    const double cursor_height = static_cast<double>(cursor.height) / PANGO_SCALE;
    chat->editor_scroll =
        std::max(chat->editor_scroll, cursor_y + cursor_height - chat->editor.size.y + 20);
    chat->editor_scroll = std::clamp(chat->editor_scroll, 0.0, cursor_y);
    const Vec2 text_origin{chat->editor.origin.x + 12,
                           chat->editor.origin.y + 10 - chat->editor_scroll};
    const auto screen = camera.world_to_screen(text_origin, viewport());
    renderer->clip({origin, {chat->editor.size.x * camera.zoom, chat->editor.size.y * camera.zoom}},
                   height);
    if (chat->focused)
        draw_input_selection(screen);
    renderer->draw(chat->composer.mesh, screen, camera.zoom);
    if (chat->focused) {
        renderer->rectangle({{screen.x + static_cast<double>(cursor.x) / PANGO_SCALE * camera.zoom,
                              screen.y + static_cast<double>(cursor.y) / PANGO_SCALE * camera.zoom},
                             {1, static_cast<double>(cursor.height) / PANGO_SCALE * camera.zoom}},
                            {0.2f, 0.24f, 0.3f, 1});
    }
    renderer->clear_clip();
    chat->hits.push_back({ChatHit::Kind::Input, chat->editor});
    draw_chat_questions();
}

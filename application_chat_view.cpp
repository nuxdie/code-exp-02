#include "application.h"

void Application::draw_conversation()
{
    if (!chat)
        return;
    const auto origin = camera.world_to_screen(chat->bounds.origin, viewport());
    const CanvasRect bounds{origin,
                            {chat->bounds.size.x * camera.zoom, chat->bounds.size.y * camera.zoom}};
    if (!bounds.intersects({{0, 0}, {static_cast<double>(width), static_cast<double>(height)}}))
        return;
    renderer->rectangle({{origin.x + 3, origin.y + 4}, bounds.size}, {0.1f, 0.15f, 0.2f, 0.08f});
    renderer->rectangle(bounds, {0.995f, 0.993f, 0.985f, 1});
    chat->hits.clear();
    chat->hits.push_back({ChatHit::Kind::Title, {chat->bounds.origin, {chat->bounds.size.x, 64}}});
    chat->body = {{chat->bounds.origin.x + 24, chat->bounds.origin.y + 88},
                  {chat->bounds.size.x - 48, chat->bounds.size.y - 256}};
    chat->editor = {{chat->bounds.origin.x + 20, chat->bounds.origin.y + chat->bounds.size.y - 144},
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
    if (camera.zoom < 0.25)
        return;
    prepare_chat_messages();
    if (chat->scrolling)
        drag_chat_scroll(camera.screen_to_world(mouse_position, viewport()));
    draw_chat_messages();
    draw_chat_controls();
}

void Application::draw_chat_chrome()
{
    const auto& model = chat->conversation;
    const auto label =
        model.title + "\n" + model.status + (build_status.empty() ? "" : " · " + build_status);
    update_document(chat->header, label, static_cast<int>(chat->bounds.size.x - 200), false);
    const auto point = camera.world_to_screen(
        {chat->bounds.origin.x + 24, chat->bounds.origin.y + 16}, viewport());
    renderer->draw(chat->header.mesh, point, std::max(0.7, camera.zoom));
    const CanvasRect button{
        {chat->bounds.origin.x + chat->bounds.size.x - 128, chat->bounds.origin.y + 18}, {104, 28}};
    auto& document = chat->messages["__threads"];
    update_document(document, "Conversations", 110, false);
    renderer->draw(document.mesh, camera.world_to_screen(button.origin, viewport()), camera.zoom);
    chat->hits.push_back({ChatHit::Kind::Threads, button});
    if (!model.error.empty()) {
        auto& error = chat->messages["__error"];
        update_document(error, model.error, static_cast<int>(chat->body.size.x), false);
        const auto point = camera.world_to_screen(
            {chat->bounds.origin.x + 24, chat->bounds.origin.y + 88}, viewport());
        renderer->clip({point, {chat->body.size.x * camera.zoom, 90 * camera.zoom}}, height);
        renderer->draw(error.mesh, point, camera.zoom);
        renderer->clear_clip();
    }
}

void Application::draw_chat_messages()
{
    const auto screen = camera.world_to_screen(chat->body.origin, viewport());
    renderer->clip({screen, {chat->body.size.x * camera.zoom, chat->body.size.y * camera.zoom}},
                   height);
    if (chat->choosing_thread) {
        double y = 0;
        for (std::size_t i = 0; i < chat->conversation.threads.size(); ++i) {
            const auto& thread = chat->conversation.threads[i];
            const auto name = thread.contains("name") && thread["name"].is_string()
                                  ? thread["name"].get<std::string>()
                                  : thread.value("preview", "Conversation");
            auto& entry = chat->messages["__thread_" + std::to_string(i)];
            update_document(entry, name.substr(0, 120), static_cast<int>(chat->body.size.x), false);
            const Vec2 world{chat->body.origin.x, chat->body.origin.y + y};
            renderer->draw(entry.mesh, camera.world_to_screen(world, viewport()), camera.zoom);
            chat->hits.push_back({ChatHit::Kind::Thread, {world, {chat->body.size.x, 48}}, i});
            y += 48;
        }
    } else {
        double y = chat->body.origin.y - chat->scroll;
        for (std::size_t i = 0; i < chat->conversation.entries.size(); ++i) {
            const auto& entry = chat->conversation.entries[i];
            const double entry_height = chat_entry_height(i);
            const CanvasRect bounds{{chat->body.origin.x, y}, {chat->body.size.x, entry_height}};
            const auto found = chat->messages.find(entry.id);
            if (bounds.intersects(chat->body) && found != chat->messages.end() &&
                found->second.layout) {
                renderer->draw(found->second.mesh,
                               camera.world_to_screen(bounds.origin, viewport()), camera.zoom);
                chat->hits.push_back({ChatHit::Kind::Message, bounds, i});
            }
            y += entry_height;
        }
    }
    renderer->clear_clip();
    draw_chat_scrollbar();
}

void Application::draw_chat_controls()
{
    const auto origin = camera.world_to_screen(chat->editor.origin, viewport());
    renderer->rectangle(
        {origin, {chat->editor.size.x * camera.zoom, chat->editor.size.y * camera.zoom}},
        chat->focused ? std::array<float, 4>{1, 1, 1, 1}
                      : std::array<float, 4>{0.97f, 0.97f, 0.96f, 1});
    const bool placeholder = chat->input.text.empty();
    update_document(chat->composer, placeholder ? "Message the agent…" : chat->input.text,
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
    const auto y = chat->bounds.origin.y + chat->bounds.size.y - 36;
    const CanvasRect send{{chat->bounds.origin.x + chat->bounds.size.x - 105, y}, {80, 28}};
    const auto& model = chat->conversation;
    const bool replying =
        !model.questions.empty() && model.questions.front().method == "item/tool/requestUserInput";
    const std::string label = model.sending                ? "Sending…"
                              : replying                   ? "Reply"
                              : !model.active_turn.empty() ? "Steer ↵"
                                                           : "Send ↵";
    auto& button = chat->messages["__send"];
    update_document(button, label, 100, false);
    renderer->draw(button.mesh, camera.world_to_screen(send.origin, viewport()), camera.zoom);
    chat->hits.push_back({ChatHit::Kind::Send, send});
    if (!model.active_turn.empty()) {
        const CanvasRect stop{{send.origin.x - 70, y}, {60, 28}};
        auto& stop_text = chat->messages["__stop"];
        update_document(stop_text, "Stop", 60, false);
        renderer->draw(stop_text.mesh, camera.world_to_screen(stop.origin, viewport()),
                       camera.zoom);
        chat->hits.push_back({ChatHit::Kind::Stop, stop});
    }
    auto& hint = chat->messages["__hint"];
    update_document(hint, "Wheel zooms · Drag scrollbar to scroll · Enter sends", 520, false);
    renderer->draw(hint.mesh, camera.world_to_screen({chat->editor.origin.x, y}, viewport()),
                   camera.zoom);
    draw_chat_questions();
}

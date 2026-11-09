#include "application.h"

ChatCanvas* Application::chat_at(Vec2 pointer)
{
    const auto world = camera.screen_to_world(pointer, viewport());
    auto contains = [&](const auto& card) {
        return card && card->visible &&
               ((!card->composing &&
                 Viewport{card->header_bounds.origin.x, card->header_bounds.origin.y,
                          card->header_bounds.size.x, card->header_bounds.size.y}
                     .contains(pointer)) ||
                Viewport{card->bounds.origin.x, card->bounds.origin.y, card->bounds.size.x,
                         card->bounds.size.y}
                    .contains(world));
    };
    if (contains(chat))
        return chat.get();
    for (auto card = chat_windows.rbegin(); card != chat_windows.rend(); ++card)
        if (contains(*card))
            return card->get();
    return nullptr;
}
bool Application::activate_chat(Vec2 pointer)
{
    auto* target = chat_at(pointer);
    if (!target || target == chat.get())
        return target != nullptr;
    for (auto& card : chat_windows)
        if (card.get() == target) {
            chat->focused = false;
            if (text_selection.surface == TextSurface::Chat ||
                text_selection.surface == TextSurface::Composer)
                text_selection = {};
            std::swap(chat, card);
            return true;
        }
    return false;
}
void Application::start_agent_windows()
{
    auto start = [&](ChatCanvas& card) {
        card.disabled = benchmark || std::getenv("SOURCE_CANVAS_DISABLE_AGENT");
        if (!card.disabled)
            card.client = std::make_unique<AgentClient>(
                python_executable, (executable.parent_path() / "agent_bridge.py").string());
    };
    start(*chat);
    refresh_agent_context();
    for (auto& card : chat_windows) {
        start(*card);
        std::swap(chat, card);
        refresh_agent_context();
        std::swap(chat, card);
    }
}
void Application::draw_conversation()
{
    chat_animation_due = Clock::time_point::max();
    auto occupied = header_hits;
    for (auto& card : chat_windows) {
        std::swap(chat, card);
        draw_chat_card();
        draw_chat_header(occupied);
        std::swap(chat, card);
    }
    draw_chat_card();
    draw_chat_header(occupied);
}
void Application::poll_agent()
{
    for (auto& card : chat_windows) {
        std::swap(chat, card);
        poll_agent_window();
        std::swap(chat, card);
    }
    poll_agent_window();
}
void Application::capture_intents()
{
    for (auto& card : chat_windows) {
        std::swap(chat, card);
        capture_window_intents();
        std::swap(chat, card);
    }
    capture_window_intents();
}
Application::Clock::time_point Application::agent_reconnect_due()
{
    auto due = chat ? chat->reconnect : Clock::time_point::max();
    for (const auto& card : chat_windows)
        due = std::min(due, card->reconnect);
    return due;
}
bool Application::agents_pending()
{
    auto pending = [](const auto& card) {
        return card && (!card->conversation.questions.empty() || card->conversation.sending);
    };
    return pending(chat) || std::any_of(chat_windows.begin(), chat_windows.end(), pending);
}
void Application::spawn_agent_window(Vec2 world, lsp::Json context)
{
    auto next = std::make_unique<ChatCanvas>(workspace_root);
    next->bounds = {world, {520, 90}};
    next->composing = true;
    next->placed = true;
    next->context = std::move(context);
    next->conversation.fresh = true;
    next->conversation.title = "New conversation";
    next->disabled = benchmark || std::getenv("SOURCE_CANVAS_DISABLE_AGENT");
    if (!next->disabled)
        next->client = std::make_unique<AgentClient>(
            python_executable, (executable.parent_path() / "agent_bridge.py").string());
    if (drag_button)
        XUngrabPointer(display, CurrentTime);
    drag_button = 0;
    text_selection = {};
    chat->focused = chat->dragging = chat->scrolling = false;
    chat_windows.push_back(std::move(chat));
    chat = std::move(next);
    refresh_agent_context();
    chat->focused = true;
    initial_view = false;
    hide_popup();
    redraw = true;
    content_size.x = std::max(content_size.x, world.x + chat->bounds.size.x);
    content_size.y = std::max(content_size.y, world.y + chat->bounds.size.y);
    schedule_save();
}
void Application::dismiss_empty_chat(const XEvent& event, Vec2 pointer)
{
    const bool canvas_click = event.xbutton.button >= Button1 && event.xbutton.button <= Button3;
    if (!drag_button && canvas_click && chat && chat->visible && chat->composing &&
        chat->input.text.find_first_not_of(" \t\r\n") == std::string::npos &&
        chat_at(pointer) != chat.get())
        close_agent_window();
}
void Application::close_agent_window()
{
    if (!chat->conversation.questions.empty() || chat->conversation.sending) {
        chat->conversation.error = "Answer the pending agent request before closing this card.";
        return;
    }
    chat->visible = false;
    chat->focused = false;
    if (text_selection.surface == TextSurface::Chat ||
        text_selection.surface == TextSurface::Composer)
        text_selection = {};
    for (auto& card : chat_windows)
        if (card->visible) {
            std::swap(chat, card);
            break;
        }
    redraw = true;
    schedule_save();
}

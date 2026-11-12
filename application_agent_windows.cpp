#include "application.h"

// A closed conversation that has something in it stays on the canvas as its title alone.
bool Application::kept_chat(const ChatCanvas& card)
{
    return !card.visible && (!card.conversation.thread_id.empty() ||
                             card.input.text.find_first_not_of(" \t\r\n") != std::string::npos);
}

void Application::start_agent(ChatCanvas& card)
{
    if (!card.disabled && !card.client && !benchmark && !std::getenv("SOURCE_CANVAS_DISABLE_AGENT"))
        card.client = std::make_unique<AgentClient>(
            python_executable, (executable.parent_path() / "agent_bridge.py").string());
}

ChatCanvas* Application::chat_at(Vec2 pointer)
{
    if (window_at(pointer) != WindowKind::Chat)
        return nullptr;
    const auto world = camera.screen_to_world(pointer, viewport());
    auto contains = [&](const auto& card) {
        if (!card)
            return false;
        const bool title = CanvasWindowManager::contains(card->header_bounds, pointer);
        if (!card->visible)
            return title;
        return (!card->composing && title) || CanvasWindowManager::contains(card->bounds, world);
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
// Only an open conversation has an agent process behind it.
void Application::start_agent_windows()
{
    const bool disabled = benchmark || std::getenv("SOURCE_CANVAS_DISABLE_AGENT");
    chat->disabled = disabled;
    if (chat->visible)
        start_agent(*chat);
    refresh_agent_context();
    for (auto& card : chat_windows) {
        card->disabled = disabled;
        if (card->visible)
            start_agent(*card);
        std::swap(chat, card);
        refresh_agent_context();
        std::swap(chat, card);
    }
}

// A click on a kept title opens the conversation again where it was.
bool Application::reopen_chat(Vec2 pointer)
{
    if (chat_at(pointer) != chat.get() || chat->visible)
        return false;
    chat->visible = true;
    start_agent(*chat);
    redraw = true;
    schedule_save();
    return true;
}
void Application::draw_conversation()
{
    chat_animation_due = Clock::time_point::max();
    auto& occupied = window_headers;
    occupied = header_hits;
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
    start_agent(*next);
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
// Closing an open conversation leaves its title on the canvas; closing the title, or a
// conversation with nothing in it, removes it for good.
void Application::close_agent_window()
{
    if (!chat->conversation.questions.empty() || chat->conversation.sending) {
        chat->conversation.error = "Answer the pending agent request before closing this card.";
        return;
    }
    const bool was_open = chat->visible;
    chat->visible = false;
    chat->focused = false;
    chat->client.reset();
    if (text_selection.surface == TextSurface::Chat ||
        text_selection.surface == TextSurface::Composer)
        text_selection = {};
    const auto open = std::find_if(chat_windows.begin(), chat_windows.end(),
                                   [](const auto& card) { return card->visible; });
    if ((!was_open || !kept_chat(*chat)) && !chat_windows.empty()) {
        const auto next = open == chat_windows.end() ? std::prev(chat_windows.end()) : open;
        chat = std::move(*next);
        chat_windows.erase(next);
    } else if (open != chat_windows.end()) {
        std::swap(chat, *open);
    }
    redraw = true;
    schedule_save();
}

// The room conversations take on the canvas: a card when open, a title when closed.
std::vector<CanvasRect> Application::conversation_bounds() const
{
    std::vector<CanvasRect> bounds;
    const auto add = [&](const ChatCanvas& card) {
        if (card.visible)
            bounds.push_back(card.bounds);
        else if (kept_chat(card))
            bounds.push_back({card.bounds.origin, {4000, 64}}); // a title stays readable far out
    };
    if (chat)
        add(*chat);
    for (const auto& card : chat_windows)
        add(*card);
    return bounds;
}

// Conversations placed under an older arrangement would land on top of files: line them
// up beside the project instead.
void Application::shelve_conversations()
{
    Vec2 next{graph.size.x + 240, 240};
    const auto place = [&](ChatCanvas& card) {
        if (!card.visible && !kept_chat(card))
            return;
        card.bounds.origin = next;
        card.placed = true;
        next.y += card.visible ? card.bounds.size.y + 96 : 64;
    };
    place(*chat);
    for (auto& card : chat_windows)
        place(*card);
}

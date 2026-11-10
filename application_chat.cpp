#include "application.h"

#include <clocale>

void Application::initialize_chat()
{
    chat = std::make_unique<ChatCanvas>(workspace_root);
    chat->disabled = benchmark || std::getenv("SOURCE_CANVAS_DISABLE_AGENT");
    load_session();
    if (!chat->placed) {
        const auto main = std::find_if(panels.begin(), panels.end(),
                                       [](const auto& panel) { return panel.path == "main.cpp"; });
        const auto origin = main == panels.end() ? Vec2{900, 0} : main->bounds.origin;
        chat->bounds.origin = {std::max(0.0, origin.x - chat->bounds.size.x - 96), origin.y};
        bool overlap = true;
        while (overlap) {
            overlap = false;
            for (const auto& panel : panels)
                if (chat->bounds.intersects(panel.bounds)) {
                    chat->bounds.origin.x = panel.bounds.origin.x + panel.bounds.size.x + 96;
                    overlap = true;
                }
        }
        chat->placed = true;
    }
    start_agent_windows();
    if (!chat->disabled && initial_view)
        focus_chat();
    std::setlocale(LC_CTYPE, "");
    XSetLocaleModifiers("");
    input_method = XOpenIM(display, nullptr, nullptr, nullptr);
    if (input_method) {
        input_context = XCreateIC(input_method, XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
                                  XNClientWindow, window, XNFocusWindow, window, nullptr);
        if (input_context)
            XSetICFocus(input_context);
    }
    std::vector<CanvasRect> bounds;
    for (const auto& panel : panels)
        bounds.push_back(panel.bounds);
    if (chat->visible)
        bounds.push_back(chat->bounds);
    for (const auto& card : chat_windows)
        if (card->visible)
            bounds.push_back(card->bounds);
    content_size = canvas_size(bounds);
}
void Application::focus_chat()
{
    if (!chat)
        return;
    camera.zoom = std::clamp(
        std::min((width - 80.0) / chat->bounds.size.x, (height - 48.0) / chat->bounds.size.y), 0.4,
        1.0);
    camera.center = {chat->bounds.origin.x + chat->bounds.size.x / 2,
                     chat->bounds.origin.y + chat->bounds.size.y / 2};
    zoom_animation.cancel(camera);
    initial_view = false;
    chat->visible = true;
    chat->focused = true;
    chat->follow = true;
    hide_popup();
    redraw = true;
}
void Application::flush_agent()
{
    if (chat && chat->client)
        for (auto& message : chat->conversation.outgoing())
            chat->client->send(std::move(message));
}
void Application::poll_agent_window()
{
    if (!chat || chat->disabled)
        return;
    if (chat->client) {
        for (const auto& message : chat->client->take_messages()) {
            const auto title = chat->conversation.title;
            chat->conversation.receive(message);
            if (chat->conversation.title != title)
                schedule_save();
            if (message.value("method", "") == "canvas/disconnected")
                chat->reconnect = Clock::now() + std::chrono::seconds(3);
            redraw = true;
        }
    }
    if (!chat->conversation.accepted_text.empty()) {
        if (chat->input.text == chat->conversation.accepted_text)
            chat->input.set("");
        chat->conversation.accepted_text.clear();
        chat->follow = true;
        schedule_save();
    }
    if (chat->reconnect <= Clock::now()) {
        chat->client = std::make_unique<AgentClient>(
            python_executable, (executable.parent_path() / "agent_bridge.py").string());
        chat->reconnect = Clock::time_point::max();
    }
    render_agent_context();
    if (!chat->visible && !chat->conversation.questions.empty())
        chat->visible = true;
    flush_agent();
}

std::string Application::chat_entry_source(std::size_t index)
{
    const auto& entry = chat->conversation.entries[index];
    return entry.activity ? "" : entry.text;
}
double Application::chat_entry_height(std::size_t index)
{
    const auto& entry = chat->conversation.entries[index];
    if (entry.activity)
        return 0;
    const auto found = chat->messages.find(entry.id);
    if (found != chat->messages.end() && found->second.layout)
        return found->second.size.y + 32;
    const auto lines = std::count(entry.text.begin(), entry.text.end(), '\n') + 1;
    const auto wrapped = entry.text.size() / 75;
    return static_cast<double>(lines + wrapped + 1) * 24 + 28;
}
void Application::restore_chat_scroll(bool finish)
{
    if (chat->restore_anchor.empty() || chat->follow || chat->conversation.history_loading)
        return;
    double y = 0;
    for (std::size_t i = 0; i < chat->conversation.entries.size(); ++i) {
        const auto& entry = chat->conversation.entries[i];
        if (entry.id == chat->restore_anchor) {
            chat->scroll = y + chat->restore_offset;
            if (finish)
                chat->restore_anchor.clear();
            return;
        }
        y += chat_entry_height(i);
    }
}

void Application::prepare_chat_messages()
{
    chat->content_height = chat->working_height();
    for (std::size_t i = 0; i < chat->conversation.entries.size(); ++i)
        chat->content_height += chat_entry_height(i);
    if (chat->follow) {
        double remaining = chat->body.size.y + 200;
        for (std::size_t i = chat->conversation.entries.size(); i > 0 && remaining > 0; --i) {
            const auto& entry = chat->conversation.entries[i - 1];
            if (entry.activity)
                continue;
            update_document(chat->messages[entry.id], chat_entry_source(i - 1),
                            chat->message_width());
            remaining -= chat_entry_height(i - 1);
        }
        chat->content_height = chat->working_height();
        for (std::size_t i = 0; i < chat->conversation.entries.size(); ++i)
            chat->content_height += chat_entry_height(i);
    }
    if (chat->follow)
        chat->scroll = std::max(0.0, chat->content_height - chat->body.size.y);
    restore_chat_scroll(false);
    double y = 0;
    for (std::size_t i = 0; i < chat->conversation.entries.size(); ++i) {
        const auto& entry = chat->conversation.entries[i];
        if (entry.activity)
            continue;
        const auto height = chat_entry_height(i);
        if (y + height >= chat->scroll - 200 && y <= chat->scroll + chat->body.size.y + 200) {
            const auto source = chat_entry_source(i);
            update_document(chat->messages[entry.id], source, chat->message_width());
        }
        y += chat_entry_height(i);
    }
    chat->content_height = y + chat->working_height();
    restore_chat_scroll(true);
    const double maximum = std::max(0.0, chat->content_height - chat->body.size.y);
    chat->scroll = chat->follow ? maximum : std::clamp(chat->scroll, 0.0, maximum);
}

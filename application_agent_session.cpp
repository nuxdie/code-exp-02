#include "application.h"

lsp::Json Application::save_agent_window()
{
    chat->drafts[chat->conversation.thread_id] = chat->input.text;
    std::string anchor;
    double anchor_offset = 0, offset = 0;
    for (std::size_t i = 0; i < chat->conversation.entries.size(); ++i) {
        const auto size = chat_entry_height(i);
        if (offset + size > chat->scroll) {
            anchor = chat->conversation.entries[i].id;
            anchor_offset = chat->scroll - offset;
            break;
        }
        offset += size;
    }
    return {{"threadId", chat->conversation.thread_id},
            {"title", chat->conversation.title},
            {"fresh", chat->conversation.fresh},
            {"drafts", chat->drafts},
            {"draft", chat->input.text},
            {"cursor", chat->input.cursor},
            {"anchor", chat->input.anchor},
            {"chatFocused", chat->focused},
            {"chatScroll", chat->scroll},
            {"chatFollow", chat->follow},
            {"chatAnchor", anchor},
            {"chatOffset", anchor_offset},
            {"chat", {{"x", chat->bounds.origin.x}, {"y", chat->bounds.origin.y}}},
            {"visible", chat->visible},
            {"context", chat->context}};
}
void Application::load_agent_window(ChatCanvas& card, const lsp::Json& state)
{
    card.conversation.thread_id = state.value("threadId", card.conversation.thread_id);
    card.conversation.title = state.value("title", card.conversation.title);
    card.conversation.fresh = state.value("fresh", false);
    card.input.set(state.value("draft", ""));
    card.drafts = state.value("drafts", std::map<std::string, std::string>{});
    card.input.place(state.value("cursor", card.input.text.size()));
    card.input.anchor = std::min(state.value("anchor", card.input.cursor), card.input.text.size());
    card.focused = state.value("chatFocused", false);
    card.scroll = state.value("chatScroll", 0.0);
    card.follow = state.value("chatFollow", true);
    card.restore_anchor = state.value("chatAnchor", "");
    card.restore_offset = state.value("chatOffset", 0.0);
    card.context = state.value("context", lsp::Json::object());
    card.visible = state.value("visible", true);
    const auto bounds = state.value("chat", lsp::Json::object());
    if (bounds.contains("x")) {
        card.bounds.origin = {bounds.at("x"), bounds.at("y")};
        card.placed = true;
    }
}

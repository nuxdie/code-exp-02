#pragma once

#include "agent_client.h"
#include "conversation.h"
#include "document_view.h"
#include "text_input.h"

#include <chrono>
#include <map>

struct ChatHit {
    enum class Kind { Title, Input, Send, Stop, Threads, Thread, Message, Accept, Decline, Option };
    Kind kind;
    CanvasRect bounds;
    std::size_t index = 0;
};
struct ChatCanvas {
    explicit ChatCanvas(const std::string& root) : conversation(root) {}
    Conversation conversation;
    std::unique_ptr<AgentClient> client;
    CanvasRect bounds{{0, 0}, {760, 900}};
    CanvasRect body{}, editor{};
    std::map<std::string, DocumentView> messages;
    DocumentView header, composer, actions, status;
    std::vector<ChatHit> hits;
    TextInput input;
    std::map<std::string, std::string> drafts;
    double scroll = 0, content_height = 0;
    double editor_scroll = 0;
    std::string restore_anchor;
    double restore_offset = 0;
    std::size_t question_index = 0;
    nlohmann::json question_answers = nlohmann::json::object();
    std::string question_key;
    bool focused = false, follow = true, choosing_thread = false, placed = false;
    bool dragging = false;
    bool disabled = false;
    std::chrono::steady_clock::time_point reconnect = std::chrono::steady_clock::time_point::max();
};

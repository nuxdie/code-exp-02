#pragma once

#include "agent_client.h"
#include "conversation.h"
#include "document_view.h"
#include "text_input.h"

#include <algorithm>
#include <chrono>
#include <map>

struct ChatHit {
    enum class Kind {
        Title,
        Input,
        Send,
        Stop,
        Threads,
        Thread,
        Scrollbar,
        Message,
        Accept,
        Decline,
        Option,
        Close
    };
    Kind kind;
    CanvasRect bounds;
    std::size_t index = 0;
};
struct ChatCanvas {
    explicit ChatCanvas(const std::string& root) : conversation(root) {}
    Conversation conversation;
    nlohmann::json context = nlohmann::json::object();
    bool visible = true;
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
    bool dragging = false, scrolling = false;
    double scroll_grab_fraction = 0;
    CanvasRect scrollbar_track() const
    {
        if (choosing_thread || body.size.y <= 0 || content_height <= body.size.y)
            return {};
        return {{body.origin.x + body.size.x + 4, body.origin.y}, {16, body.size.y}};
    }
    CanvasRect scrollbar_thumb() const
    {
        const auto track = scrollbar_track();
        if (track.size.y <= 0)
            return {};
        const double size =
            std::min(track.size.y, std::max(24.0, body.size.y * body.size.y / content_height));
        const double fraction = std::clamp(scroll / (content_height - body.size.y), 0.0, 1.0);
        return {{track.origin.x, track.origin.y + fraction * (track.size.y - size)},
                {track.size.x, size}};
    }
    bool disabled = false;
    std::chrono::steady_clock::time_point reconnect = std::chrono::steady_clock::time_point::max();
};

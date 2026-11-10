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
        Text,
        Accept,
        Decline,
        Option
    };
    Kind kind;
    CanvasRect bounds;
    std::size_t index = 0;
    PangoLayout* text_layout = nullptr;
};
struct ChatCanvas {
    explicit ChatCanvas(const std::string& root) : conversation(root) {}
    Conversation conversation;
    nlohmann::json context = nlohmann::json::object();
    bool visible = true, composing = false;
    std::unique_ptr<AgentClient> client;
    CanvasRect bounds{{0, 0}, {760, 900}};
    CanvasRect body{}, editor{};
    CanvasRect header_bounds{}; // Screen space; labels stay readable when zoomed out.
    std::map<std::string, DocumentView> messages;
    DocumentView header, composer, actions, status, context_preview;
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
    void follow_camera(const Camera& before, const Camera& after, const Viewport& viewport)
    {
        const auto screen = before.world_to_screen(bounds.origin, viewport);
        const auto origin = after.screen_to_world(screen, viewport);
        const Vec2 delta{origin.x - bounds.origin.x, origin.y - bounds.origin.y};
        auto move = [&](CanvasRect& rect) {
            rect.origin.x += delta.x;
            rect.origin.y += delta.y;
        };
        move(bounds);
        move(body);
        move(editor);
        for (auto& hit : hits)
            move(hit.bounds);
    }
    int message_width() const { return static_cast<int>(body.size.x * 0.86 - 24); }
    double working_height() const
    {
        return conversation.connected && conversation.ready && conversation.questions.empty() &&
                       (conversation.sending || !conversation.active_turn.empty())
                   ? 36
                   : 0;
    }
    CanvasRect message_text_bounds(std::size_t index, double y) const
    {
        const auto& entry = conversation.entries[index];
        const auto found = messages.find(entry.id);
        const auto size = found == messages.end() ? Vec2{} : found->second.size;
        const double x =
            entry.role == "You" ? body.origin.x + body.size.x - size.x - 12 : body.origin.x + 12;
        return {{x, y + 10}, size};
    }
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

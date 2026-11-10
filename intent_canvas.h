#pragma once

#include "agent_client.h"
#include "document_view.h"
#include "text_input.h"

#include <chrono>
#include <map>
#include <set>

struct IntentCanvas {
    struct Badge {
        CanvasRect bounds;
        std::string path;
    };
    std::unique_ptr<AgentClient> client;
    CanvasRect bounds{{0, 0}, {640, 800}}, body{}, editor{};
    DocumentView header, document, controls, rationale;
    std::map<std::string, DocumentView> badges;
    std::map<std::string, std::vector<std::string>> file_records;
    std::vector<Badge> badge_hits;
    nlohmann::json records = nlohmann::json::array(), evidence = nlohmann::json::object();
    std::map<int, std::string> pending;
    std::set<std::string> captured;
    TextInput input;
    std::string selected, error, token, path, anchor_path;
    int next_id = 1;
    double scroll = 0, anchor_offset = 0;
    bool ready = false, overview = true, editing = false, dragging = false, placed = false;
    bool visible = false, scrolling = false;
    double scroll_grab_fraction = 0;

    CanvasRect scrollbar_track() const
    {
        return {{bounds.origin.x + bounds.size.x - 20, body.origin.y},
                {16, document.size.y > body.size.y ? body.size.y : 0}};
    }
    CanvasRect scrollbar_thumb() const
    {
        return canvas_scrollbar_thumb(scrollbar_track(), document.size.y, scroll);
    }
    void drag_scroll(Vec2 world)
    {
        scroll = canvas_scrollbar_drag(scrollbar_track(), document.size.y, scroll, world.y,
                                       scroll_grab_fraction);
    }
    std::chrono::steady_clock::time_point refresh = std::chrono::steady_clock::time_point::max();
};

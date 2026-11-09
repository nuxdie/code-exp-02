#pragma once

#include "agent_client.h"
#include "document_view.h"
#include "text_input.h"

#include <chrono>
#include <map>
#include <set>

struct IntentCanvas {
    std::unique_ptr<AgentClient> client;
    CanvasRect bounds{{0, 0}, {820, 940}}, body{}, editor{};
    DocumentView header, document, controls, rationale;
    nlohmann::json records = nlohmann::json::array(), evidence = nlohmann::json::object();
    std::map<int, std::string> pending;
    std::set<std::string> captured;
    TextInput input;
    std::string selected, error, token;
    int next_id = 1;
    double scroll = 0;
    bool ready = false, overview = false, editing = false, dragging = false, placed = false;
    bool visible = false;
    std::chrono::steady_clock::time_point refresh = std::chrono::steady_clock::time_point::max();
};

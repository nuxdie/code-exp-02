#include "application.h"

void Application::initialize_intent()
{
    intent = std::make_unique<IntentCanvas>();
    if (benchmark || std::getenv("SOURCE_CANVAS_DISABLE_INTENT"))
        return;
    intent->bounds.origin = {chat->bounds.origin.x,
                             chat->bounds.origin.y + chat->bounds.size.y + 96};
    bool overlap = true;
    while (overlap) {
        overlap = false;
        for (const auto& panel : panels)
            if (intent->bounds.intersects(panel.bounds)) {
                intent->bounds.origin.x = panel.bounds.origin.x + panel.bounds.size.x + 96;
                overlap = true;
            }
    }
    if (intent_session.contains("x")) {
        intent->bounds.origin = {intent_session.value("x", 0.0), intent_session.value("y", 0.0)};
        intent->selected = intent_session.value("selected", "");
        intent->overview = intent_session.value("overview", false);
        intent->scroll = intent_session.value("scroll", 0.0);
    }
    intent->placed = true;
    intent->client = std::make_unique<AgentClient>(
        python_executable, (executable.parent_path() / "provenance.py").string());
    intent_request("initialize", {{"root", workspace_root}});
}
void Application::intent_request(const std::string& method, lsp::Json params)
{
    if (!intent || !intent->client)
        return;
    const auto id = intent->next_id++;
    intent->pending[id] = method;
    intent->client->send({{"id", id}, {"method", method}, {"params", std::move(params)}});
}
void Application::poll_intent()
{
    if (!intent || !intent->client)
        return;
    for (const auto& message : intent->client->take_messages())
        receive_intent(message);
    if (intent->ready && intent->refresh <= Clock::now()) {
        intent_request("decisions/list");
        intent->refresh = Clock::now() + std::chrono::seconds(3);
    }
    capture_intents();
}
void Application::receive_intent(const lsp::Json& message)
{
    if (!message.contains("id") || !message["id"].is_number_integer())
        return;
    const auto found = intent->pending.find(message["id"].get<int>());
    if (found == intent->pending.end())
        return;
    const auto method = found->second;
    intent->pending.erase(found);
    redraw = true;
    if (message.contains("error")) {
        intent->error = message["error"].value("message", "Intent service failed");
        return;
    }
    if (method != "decisions/list")
        intent->error.clear();
    const auto result = message.value("result", lsp::Json::object());
    if (result.contains("decisions") && result.contains("working_paths")) {
        intent->records = result["decisions"];
        intent->ready = true;
        intent->refresh = Clock::now() + std::chrono::seconds(3);
        for (const auto& record : intent->records)
            intent->captured.insert(record.value("id", ""));
        if (intent->selected.empty() && !intent->records.empty())
            intent->selected = intent->records.back().value("id", "");
    }
    if (method == "evidence/line" || method == "evidence/commit" || method == "evidence/preview") {
        intent->evidence = result;
        if (method == "evidence/line") {
            const auto decisions = result.value("decisions", lsp::Json::array());
            intent->selected = decisions.empty() ? "" : decisions.front().value("id", "");
        }
        intent->scroll = 0;
        intent->token = result.value("token", "");
    }
    if (method == "evidence/checkpoint") {
        intent->evidence = result["evidence"];
        intent->token.clear();
        intent->scroll = 0;
    }
}
void Application::capture_window_intents()
{
    if (!intent->ready || !chat || chat->conversation.history_loading)
        return;
    for (const auto& entry : chat->conversation.entries) {
        const auto id = "decision-" + entry.id;
        if (entry.role != "You" || entry.activity || entry.text.empty() ||
            entry.text.find("<environment_context>") != std::string::npos ||
            intent->captured.count(id))
            continue;
        intent->captured.insert(id);
        intent_request("decisions/record", {{"decision",
                                             {{"id", id},
                                              {"statement", entry.text},
                                              {"origin",
                                               {{"kind", "conversation"},
                                                {"thread_id", chat->conversation.thread_id},
                                                {"turn_id", entry.turn},
                                                {"message_id", entry.id}}}}}});
    }
}
void Application::focus_intent()
{
    if (!intent || !intent->placed)
        return;
    remember_view();
    intent->visible = true;
    content_size.x = std::max(content_size.x, intent->bounds.origin.x + intent->bounds.size.x);
    content_size.y = std::max(content_size.y, intent->bounds.origin.y + intent->bounds.size.y);
    camera.zoom = std::clamp(
        std::min((width - 80.0) / intent->bounds.size.x, (height - 48.0) / intent->bounds.size.y),
        0.4, 1.0);
    camera.center = {intent->bounds.origin.x + intent->bounds.size.x / 2,
                     intent->bounds.origin.y + intent->bounds.size.y / 2};
    zoom_animation.cancel(camera);
    finish_view();
    chat->focused = false;
    hide_popup();
    redraw = true;
}
void Application::inspect_intent(Vec2 pointer)
{
    const auto hit = hit_test(pointer);
    if (hit.panel < 0 || hit.line < 0)
        return;
    intent->overview = false;
    intent_request("evidence/line", {{"path", panels[hit.panel].path}, {"line", hit.line + 1}});
    focus_intent();
}

void Application::attribute_chat_entry(std::size_t index)
{
    if (!intent || index >= chat->conversation.entries.size())
        return;
    const auto& entry = chat->conversation.entries[index];
    if (entry.activity)
        return;
    if (entry.role == "You") {
        capture_intents();
        intent->selected = "decision-" + entry.id;
    } else if (entry.role == "Agent") {
        intent_request("decisions/rationale", {{"decision_id", intent->selected},
                                               {"rationale",
                                                {{"summary", entry.text},
                                                 {"author_role", "agent"},
                                                 {"basis", "selected_public_agent_response"},
                                                 {"message_id", entry.id},
                                                 {"turn_id", entry.turn},
                                                 {"thread_id", chat->conversation.thread_id}}}});
    }
    intent->overview = false;
    intent->evidence.clear();
    intent->scroll = 0;
    focus_intent();
}

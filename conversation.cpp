#include "conversation.h"

#include <algorithm>

namespace {
std::string string_value(const nlohmann::json& object, const char* key)
{
    const auto found = object.find(key);
    return found != object.end() && found->is_string() ? found->get<std::string>() : "";
}
std::string user_text(const nlohmann::json& item)
{
    std::string text;
    for (const auto& part : item.value("content", nlohmann::json::array())) {
        if (part.value("type", "") == "text")
            text += string_value(part, "text");
        else if (part.value("type", "") == "localImage" || part.value("type", "") == "image")
            text += "\n[Attached image]\n";
    }
    return text;
}
std::string public_questions(const nlohmann::json& item)
{
    std::string text;
    if (!item.contains("questions") || !item["questions"].is_array())
        return text;
    for (const auto& question : item["questions"]) {
        text += "\n\n**" + question.value("title", "") + "**";
        for (const auto& option : question.value("options", nlohmann::json::array()))
            if (option.is_string())
                text += "\n• " + option.get<std::string>();
    }
    return text;
}
} // namespace

Conversation::Conversation(std::string root) : root(std::move(root)) {}
int Conversation::request(const std::string& method, Json params)
{
    const int id = next_id++;
    pending[id] = method;
    queued.push_back({{"id", id}, {"method", method}, {"params", std::move(params)}});
    return id;
}
std::vector<Conversation::Json> Conversation::outgoing()
{
    std::vector<Json> messages;
    messages.swap(queued);
    return messages;
}
void Conversation::receive(const Json& message)
{
    if (message.contains("method")) {
        const auto method = message.value("method", "");
        const auto params = message.value("params", Json::object());
        if (message.contains("id")) {
            if (params.contains("threadId") && params["threadId"] != thread_id)
                return;
            const auto reason = string_value(params, "reason");
            questions.push_back({message["id"], method,
                                 reason.empty() ? "Agent needs your response" : reason, params});
            ++revision;
        } else
            notification(method, params);
    } else if (message.contains("id") && message["id"].is_number_integer())
        response(message);
}
void Conversation::hydrate_thread(const Json& thread)
{
    thread_id = thread.value("id", thread_id);
    const auto name = string_value(thread, "name");
    title = name.empty() ? "Conversation" : name;
    status = thread.value("status", Json::object()).value("type", "idle") == "active"
                 ? "Agent working"
                 : "Ready";
    ready = true;
}
void Conversation::response(const Json& message)
{
    const int id = message["id"];
    const auto found = pending.find(id);
    if (found == pending.end())
        return;
    const auto method = found->second;
    pending.erase(found);
    if (message.contains("error")) {
        request_failed(method, message["error"]);
        ++revision;
        return;
    }
    const auto result = message.value("result", Json::object());
    if (method == "initialize") {
        connected = true;
        queued.push_back({{"method", "initialized"}, {"params", Json::object()}});
        if (!thread_id.empty())
            request("thread/resume", {{"threadId", thread_id}, {"excludeTurns", true}});
        else if (fresh) {
            ready = true;
            status = "Ready";
        } else
            list_threads();
    } else if (method == "thread/resume" || method == "thread/start") {
        thread_ready(method, result);
    } else if (method == "thread/turns/list") {
        history_page(result);
    } else if (method == "thread/list") {
        threads = result.value("data", std::vector<Json>{});
        if (!fresh && thread_id.empty() && !threads.empty())
            select_thread(threads.front().value("id", ""));
        else if (thread_id.empty()) {
            ready = true;
            status = "Ready";
        }
    } else if (method == "turn/start" || method == "turn/steer") {
        active_turn = method == "turn/start" ? result.value("turn", Json::object()).value("id", "")
                                             : result.value("turnId", "");
        accepted_text = std::move(submitted);
        submitted.clear();
        sending = false;
        fresh = false;
        error.clear();
        status = "Agent working";
    }
    ++revision;
}
void Conversation::thread_ready(const std::string& method, const Json& result)
{
    hydrate_thread(result.at("thread"));
    if (method == "thread/start")
        fresh = true;
    error.clear();
    history_loading = method == "thread/resume";
    if (history_loading) {
        fresh = false;
        request("thread/turns/list", {{"threadId", thread_id},
                                      {"itemsView", "full"},
                                      {"sortDirection", "asc"},
                                      {"limit", 10}});
    }
    if (!submitted.empty())
        send_prompt();
}
void Conversation::request_failed(const std::string& method, const Json& failure)
{
    const auto detail = failure.value("message", "Agent request failed");
    if (method == "thread/resume" && fresh &&
        detail.find("no rollout found for thread id ") != std::string::npos) {
        // A thread without an accepted first message has no saved history to resume.
        thread_id.clear();
        submitted.clear();
        history_loading = sending = false;
        ready = connected;
        title = "New conversation";
        status = "Ready";
        error.clear();
        return;
    }
    if (method == "thread/turns/list") {
        history_loading = false;
        if (detail.find("is not materialized yet") != std::string::npos)
            return;
    }
    error = detail;
    status = "Request failed";
    if (method == "turn/start" || method == "turn/steer" || method == "thread/start") {
        sending = false;
        submitted.clear();
    }
}
void Conversation::history_page(const Json& result)
{
    for (const auto& turn : result.value("data", Json::array())) {
        const auto id = turn.value("id", "");
        int sequence = 0;
        for (const auto& item : turn.value("items", Json::array()))
            put_item(id, item, sequence++);
        sequences[id] = std::max(sequences[id], sequence);
        if (turn.value("status", "") == "inProgress")
            active_turn = id;
    }
    std::stable_sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        return a.turn == b.turn ? a.sequence < b.sequence : a.turn < b.turn;
    });
    const auto cursor = string_value(result, "nextCursor");
    history_loading = !cursor.empty();
    if (!cursor.empty())
        request("thread/turns/list", {{"threadId", thread_id},
                                      {"itemsView", "full"},
                                      {"sortDirection", "asc"},
                                      {"limit", 10},
                                      {"cursor", cursor}});
}
void Conversation::put_item(const std::string& turn, const Json& item, int sequence)
{
    const auto type = item.value("type", "");
    // Reasoning items are internal; the canvas shows public conversation and activity only.
    if (type != "agentMessage" && type != "userMessage" && type != "commandExecution" &&
        type != "fileChange" && type != "plan")
        return;
    const auto id = item.value("id", "");
    auto found = std::find_if(entries.begin(), entries.end(),
                              [&](const auto& entry) { return entry.id == id; });
    if (found == entries.end()) {
        entries.push_back({});
        found = std::prev(entries.end());
        found->id = id;
        found->turn = turn;
        found->sequence = sequence >= 0 ? sequence : sequences[turn]++;
    } else if (sequence >= 0)
        found->sequence = sequence;
    found->activity = type == "commandExecution" || type == "fileChange";
    found->role = type == "userMessage" ? "You" : found->activity ? "Activity" : "Agent";
    if (type == "commandExecution") {
        found->text = string_value(item, "command");
        found->detail = string_value(item, "aggregatedOutput");
    } else if (type == "fileChange") {
        found->text = "Changed files";
        found->detail.clear();
        for (const auto& change : item.value("changes", Json::array()))
            found->detail += change.value("path", "") + "\n";
    } else
        found->text = type == "userMessage" ? user_text(item) : string_value(item, "text");
    if (type == "agentMessage")
        found->text += public_questions(item);
    ++found->revision;
    ++revision;
}
void Conversation::delta(const Json& params, bool output)
{
    const auto id = params.value("itemId", "");
    auto found = std::find_if(entries.begin(), entries.end(),
                              [&](const auto& entry) { return entry.id == id; });
    if (found == entries.end() && !output) {
        put_item(params.value("turnId", ""), {{"type", "agentMessage"}, {"id", id}, {"text", ""}});
        found = std::prev(entries.end());
    }
    if (found != entries.end()) {
        (output ? found->detail : found->text) += params.value("delta", "");
        ++found->revision;
        ++revision;
    }
}
void Conversation::notification(const std::string& method, const Json& params)
{
    if (method == "canvas/connected") {
        pending.clear();
        connected = ready = false;
        status = "Connecting…";
        request("initialize",
                {{"clientInfo",
                  {{"name", "source_canvas"}, {"title", "Source Canvas"}, {"version", "0.2"}}},
                 {"capabilities", {{"experimentalApi", true}}}});
        return;
    }
    if (method == "canvas/disconnected") {
        queued.clear();
        pending.clear();
        connected = ready = sending = false;
        submitted.clear();
        error = params.value("message", "Agent connection closed");
        status = "Disconnected · reconnecting…";
        ++revision;
        return;
    }
    if (params.contains("threadId") && params["threadId"] != thread_id)
        return;
    if (method == "item/started" || method == "item/completed")
        put_item(params.value("turnId", ""), params.value("item", Json::object()));
    else if (method == "item/agentMessage/delta")
        delta(params, false);
    else if (method == "item/commandExecution/outputDelta")
        delta(params, true);
    else if (method == "turn/started") {
        active_turn = params.value("turn", Json::object()).value("id", "");
        status = "Agent working";
    } else if (method == "turn/completed") {
        const auto turn = params.value("turn", Json::object());
        active_turn.clear();
        status = turn.value("status", "") == "failed" ? "Agent failed" : "Ready";
        if (turn.contains("error") && turn["error"].is_object())
            error = turn["error"].value("message", "Agent turn failed");
    } else if (method == "thread/name/updated" && string_value(params, "threadId") == thread_id) {
        const auto name = string_value(params, "threadName");
        title = name.empty() ? "Conversation" : name;
    } else if (method == "thread/status/changed") {
        const auto state = params.value("status", Json::object()).value("type", "");
        if (state == "idle") {
            active_turn.clear();
            status = "Ready";
        }
    }
    ++revision;
}
void Conversation::send_prompt()
{
    Json params{{"threadId", thread_id},
                {"input", Json::array({{{"type", "text"}, {"text", submitted}}})}};
    if (!canvas_context.empty())
        params["additionalContext"] = {
            {"source-canvas",
             {{"kind", "untrusted"}, {"name", "canvas_context"}, {"value", canvas_context}}}};
    const bool active = !active_turn.empty();
    if (active)
        params["expectedTurnId"] = active_turn;
    request(active ? "turn/steer" : "turn/start", std::move(params));
}
bool Conversation::submit(const std::string& text)
{
    if (!ready || sending || text.find_first_not_of(" \t\n") == std::string::npos)
        return false;
    sending = true;
    error.clear();
    submitted = text;
    if (thread_id.empty())
        request("thread/start",
                {{"cwd", root}, {"approvalPolicy", "never"}, {"sandbox", "danger-full-access"}});
    else
        send_prompt();
    return true;
}
void Conversation::interrupt()
{
    if (!active_turn.empty())
        request("turn/interrupt", {{"threadId", thread_id}, {"turnId", active_turn}});
}
void Conversation::select_thread(const std::string& id)
{
    if (!questions.empty()) {
        error = "Answer the pending agent request before switching conversations.";
        return;
    }
    pending.clear();
    if (connected && !thread_id.empty() && thread_id != id)
        request("thread/unsubscribe", {{"threadId", thread_id}});
    sending = false;
    submitted.clear();
    accepted_text.clear();
    thread_id = id;
    fresh = false;
    active_turn.clear();
    entries.clear();
    questions.clear();
    sequences.clear();
    ready = false;
    error.clear();
    request("thread/resume", {{"threadId", id}, {"excludeTurns", true}});
    ++revision;
}
void Conversation::list_threads()
{
    request("thread/list", {{"cwd", root}, {"limit", 50}, {"sortKey", "updated_at"}});
}
void Conversation::answer(std::size_t index, bool accept, const std::string& text)
{
    if (index >= questions.size())
        return;
    const auto question = questions[index];
    Json result{{"decision", accept ? "accept" : "decline"}};
    if (question.method == "item/tool/requestUserInput") {
        Json answers = Json::object();
        for (const auto& prompt : question.params.value("questions", Json::array()))
            answers[prompt.value("id", "")] = {{"answers", Json::array({text})}};
        result = {{"answers", answers}};
    }
    queued.push_back({{"id", question.id}, {"result", result}});
    questions.erase(questions.begin() + static_cast<long>(index));
    ++revision;
}

void Conversation::answer_questions(std::size_t index, Json answers)
{
    if (index >= questions.size())
        return;
    queued.push_back({{"id", questions[index].id}, {"result", {{"answers", std::move(answers)}}}});
    questions.erase(questions.begin() + static_cast<long>(index));
    ++revision;
}

#include "agent_client.h"
#include "conversation.h"
#include "text_input.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <poll.h>
#include <regex>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

using Json = nlohmann::json;
void expect(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}
Json take_request(Conversation& model, const std::string& method)
{
    for (const auto& message : model.outgoing())
        if (message.value("method", "") == method)
            return message;
    throw std::runtime_error("Missing request: " + method);
}
void test_conversation()
{
    Conversation model("/project");
    model.thread_id = "thread";
    model.receive({{"method", "canvas/connected"}, {"params", Json::object()}});
    auto request = take_request(model, "initialize");
    model.receive({{"id", request["id"]}, {"result", Json::object()}});
    request = take_request(model, "thread/resume");
    expect(request["params"].size() == 2 && request["params"]["threadId"] == "thread",
           "Attaching must preserve the existing model, permissions and agent configuration");
    model.receive(
        {{"id", request["id"]},
         {"result",
          {{"thread",
            {{"id", "thread"}, {"name", "Code canvas"}, {"status", {{"type", "active"}}}}}}}});
    request = take_request(model, "thread/turns/list");
    const Json items = Json::array(
        {{{"id", "user"},
          {"type", "userMessage"},
          {"content", Json::array({{{"type", "text"}, {"text", "Hello 🌍"}}})}},
         {{"id", "internal"}, {"type", "reasoning"}, {"content", {"private analysis sentinel"}}},
         {{"id", "answer"}, {"type", "agentMessage"}, {"text", "Working"}}});
    model.receive(
        {{"id", request["id"]},
         {"result",
          {{"data", Json::array({{{"id", "turn"}, {"status", "inProgress"}, {"items", items}}})},
           {"nextCursor", nullptr}}}});
    expect(model.entries.size() == 2 && model.entries[0].text == "Hello 🌍" &&
               model.active_turn == "turn",
           "History must retain public messages, Unicode and the active turn while omitting "
           "reasoning");
    model.receive(
        {{"method", "item/agentMessage/delta"},
         {"params",
          {{"threadId", "thread"}, {"turnId", "turn"}, {"itemId", "answer"}, {"delta", " now"}}}});
    expect(model.entries.back().text == "Working now",
           "Streamed deltas must update the existing message");
    expect(model.submit("Please focus on rendering"),
           "The composer must submit while the agent is working");
    request = take_request(model, "turn/steer");
    expect(request["params"]["expectedTurnId"] == "turn",
           "Steering must target the active turn explicitly");
    model.receive({{"id", request["id"]}, {"result", {{"turnId", "turn"}}}});
    expect(model.accepted_text == "Please focus on rendering",
           "Only acknowledged input may be cleared from the draft");
    model.interrupt();
    request = take_request(model, "turn/interrupt");
    expect(request["params"]["turnId"] == "turn",
           "Stopping must target the same active conversation");
    model.receive({{"id", "approval"},
                   {"method", "item/commandExecution/requestApproval"},
                   {"params", {{"reason", nullptr}, {"command", "cmake --build build"}}}});
    model.answer(0, false);
    const auto replies = model.outgoing();
    expect(
        replies.size() == 1 && replies[0]["id"] == "approval" &&
            replies[0]["result"]["decision"] == "decline",
        "Actual approvals must receive the user's explicit answer, including string request IDs");
    model.receive({{"method", "turn/completed"},
                   {"params", {{"threadId", "thread"}, {"turn", {{"status", "completed"}}}}}});
    expect(model.submit("Next change"), "An idle conversation must accept another turn");
    request = take_request(model, "turn/start");
    expect(!request["params"].contains("model"), "New turns must inherit the current model");
    model.receive({{"method", "canvas/disconnected"}, {"params", {{"message", "offline"}}}});
    expect(!model.ready && model.outgoing().empty(),
           "Reconnect must not replay an unacknowledged message");
}
void test_contextual_conversation()
{
    Conversation model("/project");
    model.fresh = true;
    model.canvas_context = "File: camera.h:42\nSelected: camera.zoom";
    model.receive({{"method", "canvas/connected"}, {"params", Json::object()}});
    auto request = take_request(model, "initialize");
    model.receive({{"id", request["id"]}, {"result", Json::object()}});
    const auto handshake = model.outgoing();
    expect(model.ready && handshake.size() == 1 && handshake.front()["method"] == "initialized",
           "A new card must remain a fresh conversation rather than attach to recent history");
    model.list_threads();
    request = take_request(model, "thread/list");
    model.receive(
        {{"id", request["id"]}, {"result", {{"data", Json::array({{{"id", "recent-thread"}}})}}}});
    expect(model.thread_id.empty(),
           "Listing old conversations must not attach a fresh card without a selection");
    model.instructions = "- wheel-zooms: The wheel zooms everywhere.";
    expect(model.submit("Make this smoother"), "A connected fresh card must accept a prompt");
    request = take_request(model, "thread/start");
    expect(request["params"]["developerInstructions"] == model.instructions &&
               request["params"]["dynamicTools"][1]["name"] == "canvas_intent",
           "New agents must be told the standing decisions and be able to file their turn");
    expect(request["params"]["cwd"] == "/project", "New cards must retain the project workspace");
    expect(request["params"]["approvalPolicy"] == "never" &&
               request["params"]["sandbox"] == "danger-full-access",
           "New canvas agents must start with no approval prompts and full access");
    const auto& tool = request["params"]["dynamicTools"][0];
    expect(tool["name"] == "canvas_walkthrough" && tool["type"] == "function" &&
               tool["inputSchema"]["properties"].contains("steps"),
           "New canvas threads must advertise the native code walkthrough tool");
    model.receive({{"id", request["id"]}, {"result", {{"thread", {{"id", "new-thread"}}}}}});
    const auto first_turn = model.outgoing();
    expect(first_turn.size() == 1 && first_turn.front()["method"] == "turn/start" &&
               model.sending && !model.history_loading,
           "A fresh thread must send its first turn without reading nonexistent persisted history");
    request = first_turn.front();
    const auto& params = request["params"];
    expect(params["threadId"] == "new-thread" && params["input"][0]["text"] == "Make this smoother",
           "The human's message must remain exact and go to the card's own thread");
    expect(params["additionalContext"]["source-canvas"]["kind"] == "untrusted" &&
               std::regex_match(
                   params["additionalContext"]["source-canvas"]["name"].get<std::string>(),
                   std::regex("[a-z][a-z0-9_]{0,63}")) &&
               params["additionalContext"]["source-canvas"]["value"] == model.canvas_context,
           "Source and selection context must be attached separately as project data");
    model.receive({{"id", request["id"]}, {"result", {{"turn", {{"id", "new-turn"}}}}}});
    expect(model.accepted_text == "Make this smoother" && !model.fresh && model.error.empty(),
           "Context must not prevent clearing an acknowledged draft");
    model.canvas_context = "Updated source snapshot";
    expect(model.submit("Continue"), "The context card must support steering");
    request = take_request(model, "turn/steer");
    expect(request["params"]["additionalContext"]["source-canvas"]["value"] == model.canvas_context,
           "Steering must send the updated location context too");
    model.receive({{"id", "other-approval"},
                   {"method", "item/commandExecution/requestApproval"},
                   {"params", {{"threadId", "another-thread"}}}});
    expect(model.questions.empty(), "Other cards' approvals must not enter this conversation");
}
void test_unsent_thread_recovery()
{
    Conversation model("/project");
    model.fresh = true;
    model.thread_id = "unmaterialized-thread";
    model.receive({{"method", "canvas/connected"}, {"params", Json::object()}});
    auto request = take_request(model, "initialize");
    model.receive({{"id", request["id"]}, {"result", Json::object()}});
    request = take_request(model, "thread/resume");
    model.receive({{"id", request["id"]},
                   {"error",
                    {{"code", -32600},
                     {"message", "no rollout found for thread id unmaterialized-thread"}}}});
    expect(
        model.ready && model.fresh && model.thread_id.empty() && model.error.empty() &&
            model.outgoing().empty(),
        "Reloading an unsent card must recover without replaying its draft or selecting history");
    expect(model.submit("Keep my original draft"),
           "The recovered card must accept its first message");
    request = take_request(model, "thread/start");
    model.receive(
        {{"id", request["id"]}, {"result", {{"thread", {{"id", "replacement-thread"}}}}}});
    request = take_request(model, "turn/start");
    expect(request["params"]["input"][0]["text"] == "Keep my original draft",
           "Recovery must retain the human's exact message");
    model.receive({{"id", request["id"]},
                   {"error", {{"code", -32600}, {"message", "Send rejected by server"}}}});
    expect(!model.sending && model.accepted_text.empty() &&
               model.error == "Send rejected by server",
           "A rejected send must leave the draft unacknowledged and available to retry");
    expect(model.submit("Keep my original draft"), "A rejected first turn must be retryable");
    request = take_request(model, "turn/start");
    model.receive({{"id", request["id"]}, {"result", {{"turn", {{"id", "accepted-turn"}}}}}});
    expect(model.accepted_text == "Keep my original draft" && !model.fresh && model.error.empty(),
           "Only a successful acknowledgement may clear the error and mark the thread persistent");
}
void test_history_failure_during_send()
{
    Conversation model("/project");
    model.thread_id = "existing-thread";
    model.receive({{"method", "canvas/connected"}, {"params", Json::object()}});
    auto request = take_request(model, "initialize");
    model.receive({{"id", request["id"]}, {"result", Json::object()}});
    request = take_request(model, "thread/resume");
    model.receive({{"id", request["id"]}, {"result", {{"thread", {{"id", "existing-thread"}}}}}});
    const auto history = take_request(model, "thread/turns/list");
    expect(model.submit("A pending turn"), "A resumed card must remain usable while history loads");
    const auto send = take_request(model, "turn/start");
    model.receive({{"id", history["id"]},
                   {"error",
                    {{"code", -32600},
                     {"message", "thread existing-thread is not materialized yet; "
                                 "thread/turns/list is unavailable before first user message"}}}});
    expect(model.sending && !model.history_loading && model.error.empty() &&
               !model.submit("Duplicate pending turn"),
           "A history error must not show a false send failure or allow duplicate submissions");
    model.receive({{"id", send["id"]}, {"result", {{"turn", {{"id", "accepted-turn"}}}}}});
    expect(model.accepted_text == "A pending turn" && !model.sending,
           "The actual turn acknowledgement must determine whether sending succeeded");
}
void test_input()
{
    TextInput input;
    input.set("aé🌍z");
    input.move(-1, false);
    input.erase(true);
    expect(input.text == "aéz" && input.cursor == 3,
           "Backspace must erase a whole Unicode character");
    input.move(-1, true);
    expect(input.selected() == "é", "Keyboard selection must preserve UTF-8 boundaries");
    input.insert("β\n");
    expect(input.text == "aβ\nz", "Pasting must replace the selection and retain newlines");
    input.line_end(false, false);
    expect(input.cursor == 4, "Home must move to the beginning of the current line");
    input.select_all();
    input.insert("new");
    expect(input.text == "new", "Select all and replacement must behave like a normal composer");
}
void test_thread_titles()
{
    Conversation model("/project");
    model.select_thread("thread");
    const auto request = take_request(model, "thread/resume");
    model.receive({{"id", request["id"]},
                   {"result", {{"thread", {{"id", "thread"}, {"name", "Saved title λ"}}}}}});
    expect(model.title == "Saved title λ", "Resuming must use Codex's saved thread title");
    model.receive({{"method", "thread/name/updated"},
                   {"params", {{"threadId", "other-thread"}, {"threadName", "Other title"}}}});
    expect(model.title == "Saved title λ", "Another agent's title must not rename this chat");
    const auto revision = model.revision;
    model.receive({{"method", "thread/name/updated"},
                   {"params", {{"threadId", "thread"}, {"threadName", "New Codex title 🌍"}}}});
    expect(model.title == "New Codex title 🌍" && model.revision > revision,
           "A Codex name update must refresh the chat title without waiting for a resume");
    model.receive({{"method", "thread/name/updated"},
                   {"params", {{"threadId", "thread"}, {"threadName", nullptr}}}});
    expect(model.title == "Conversation", "Clearing a Codex title must restore the default label");
    model.select_thread("untitled");
    const auto untitled = take_request(model, "thread/resume");
    model.receive({{"id", untitled["id"]},
                   {"result", {{"thread", {{"id", "untitled"}, {"name", nullptr}}}}}});
    expect(model.title == "Conversation", "Untitled threads must have a safe header label");
}
void test_bridge_error_survives_exit()
{
    const auto script = std::filesystem::temp_directory_path() /
                        ("canvas-bridge-error-" + std::to_string(getpid()));
    std::ofstream(script) << "#!/bin/sh\n"
                             "echo '{\"method\":\"canvas/disconnected\",\"params\":{"
                             "\"message\":\"Codex executable not found\"}}'\n";
    chmod(script.c_str(), 0700);
    std::vector<Json> messages;
    {
        AgentClient client(script.string(), "unused");
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (std::chrono::steady_clock::now() < deadline) {
            pollfd fd{client.event_fd(), POLLIN, 0};
            poll(&fd, 1, 50);
            for (auto& message : client.take_messages())
                messages.push_back(std::move(message));
        }
    }
    std::filesystem::remove(script);
    expect(messages.size() == 1 &&
               messages.front()["params"]["message"] == "Codex executable not found",
           "Bridge exit must preserve the specific diagnostic without a second generic error");
}
void title_user_message(Conversation& model)
{
    model.receive({{"method", "item/completed"},
                   {"params",
                    {{"threadId", model.thread_id},
                     {"turnId", "user-turn"},
                     {"item",
                      {{"id", "prompt"},
                       {"type", "userMessage"},
                       {"content", Json::array({{{"type", "text"},
                                                 {"text", "Fix Unicode rendering 🌍"}}})}}}}}});
}
Json begin_title(Conversation& model)
{
    model.thread_id = "original";
    title_user_message(model);
    auto request = take_request(model, "config/read");
    model.receive({{"id", request["id"]},
                   {"result", {{"config", {{"mcp_servers", {{"example", Json::object()}}}}}}}});
    request = take_request(model, "thread/start");
    expect(request["params"]["ephemeral"] == true && request["params"]["sandbox"] == "read-only" &&
               request["params"]["config"]["mcp_servers"]["example"]["enabled"] == false,
           "Title generation must use an ephemeral thread with tools disabled");
    return request;
}
Json complete_title(Conversation& model, const Json& start, const std::string& text,
                    const std::string& next_method = "thread/read")
{
    model.receive({{"id", start["id"]}, {"result", {{"thread", {{"id", "hidden"}}}}}});
    auto request = take_request(model, "turn/start");
    expect(request["params"]["threadId"] == "hidden" &&
               request["params"]["outputSchema"]["properties"]["title"]["maxLength"] == 36,
           "The hidden turn must request a short structured title");
    model.receive({{"id", request["id"]}, {"result", {{"turn", {{"id", "title-turn"}}}}}});
    model.receive(
        {{"method", "item/completed"},
         {"params",
          {{"threadId", "hidden"},
           {"turnId", "title-turn"},
           {"item", {{"id", "title-answer"}, {"type", "agentMessage"}, {"text", text}}}}}});
    model.receive(
        {{"method", "turn/completed"},
         {"params",
          {{"threadId", "hidden"}, {"turn", {{"id", "title-turn"}, {"status", "completed"}}}}}});
    return take_request(model, next_method);
}
void test_automatic_titles()
{
    Conversation model("/project");
    const auto start = begin_title(model);
    title_user_message(model);
    expect(model.outgoing().empty(), "Repeated input must not duplicate title generation");
    const auto read = complete_title(model, start, R"({"title":"Fix rendering 🌍"})");
    expect(model.entries.size() == 1 && model.active_turn.empty() && model.questions.empty(),
           "Hidden title activity must not affect the visible conversation");
    model.receive(
        {{"id", read["id"]}, {"result", {{"thread", {{"id", "original"}, {"name", nullptr}}}}}});
    const auto save = take_request(model, "thread/name/set");
    expect(save["params"]["threadId"] == "original" && save["params"]["name"] == "Fix rendering 🌍",
           "The generated title must be persisted on the originating thread");
    model.receive({{"id", save["id"]}, {"result", Json::object()}});
    expect(model.title == "Fix rendering 🌍", "Saving must refresh the canvas header");
    expect(take_request(model, "thread/unsubscribe")["params"]["threadId"] == "hidden",
           "Completed hidden threads must be detached");
    title_user_message(model);
    expect(model.outgoing().empty(), "Named threads must retain their existing title");
}
void test_title_races_and_failure()
{
    Conversation model("/project");
    const auto start = begin_title(model);
    const auto read = complete_title(model, start, R"({"title":"Automatic title"})");
    model.receive({{"id", read["id"]},
                   {"result", {{"thread", {{"id", "original"}, {"name", "Manual title"}}}}}});
    const auto requests = model.outgoing();
    expect(requests.size() == 1 && requests.front()["method"] == "thread/unsubscribe",
           "A saved name discovered before persistence must supersede the generated title");

    Conversation switched("/project");
    const auto late = begin_title(switched);
    switched.select_thread("other");
    switched.outgoing();
    switched.receive({{"id", late["id"]}, {"result", {{"thread", {{"id", "late-hidden"}}}}}});
    expect(take_request(switched, "thread/unsubscribe")["params"]["threadId"] == "late-hidden",
           "Switching threads must detach a hidden thread even if its start response arrives late");
    expect(switched.thread_id == "other", "Late title responses must not switch conversations");

    Conversation failed("/project");
    const auto failure = begin_title(failed);
    failed.receive({{"id", failure["id"]}, {"error", {{"message", "Unavailable"}}}});
    expect(failed.error.empty() && failed.title == "Conversation",
           "Title failures must leave normal conversation state alone");
    for (const auto* invalid : {"not JSON", R"({"title":""})", R"({"title":"line\nbreak"})",
                                R"({"title":"This title exceeds thirty-six characters"})"}) {
        Conversation malformed("/project");
        const auto hidden = begin_title(malformed);
        complete_title(malformed, hidden, invalid, "thread/unsubscribe");
        expect(malformed.title == "Conversation" && malformed.error.empty(),
               "Malformed title output must be ignored without disrupting the user");
    }
    Conversation renamed("/project");
    const auto hidden = begin_title(renamed);
    const auto pending_read = complete_title(renamed, hidden, R"({"title":"Automatic"})");
    renamed.receive({{"method", "thread/name/updated"},
                     {"params", {{"threadId", "original"}, {"threadName", "User title"}}}});
    renamed.outgoing();
    renamed.receive({{"id", pending_read["id"]},
                     {"result", {{"thread", {{"id", "original"}, {"name", nullptr}}}}}});
    expect(renamed.title == "User title" && renamed.outgoing().empty(),
           "Renaming during generation must cancel pending automatic results");
}
int main()
{
    try {
        test_bridge_error_survives_exit();
        test_conversation();
        test_input();
        test_contextual_conversation();
        test_unsent_thread_recovery();
        test_history_failure_during_send();
        test_thread_titles();
        test_automatic_titles();
        test_title_races_and_failure();
        std::cout
            << "Conversation streaming, history, steering, approvals and Unicode input passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

#include "conversation.h"
#include "text_input.h"

#include <iostream>
#include <regex>
#include <stdexcept>

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
    expect(model.submit("Make this smoother"), "A connected fresh card must accept a prompt");
    request = take_request(model, "thread/start");
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
int main()
{
    try {
        test_conversation();
        test_input();
        test_contextual_conversation();
        test_unsent_thread_recovery();
        test_history_failure_during_send();
        test_thread_titles();
        std::cout
            << "Conversation streaming, history, steering, approvals and Unicode input passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

#include "agent_client.h"
#include "conversation.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <poll.h>
#include <stdexcept>
#include <unistd.h>

namespace {
bool has_test_reply(const Conversation& model)
{
    return std::ranges::any_of(model.entries, [](const auto& entry) {
        return entry.role == "Agent" && entry.text.contains("CANVAS_SEND_OK");
    });
}
bool handle_send_test_response(const nlohmann::json& message, AgentClient& client,
                               const Conversation& model)
{
    if (message.value("id", nlohmann::json()) == 8999) {
        if (message.contains("error") ||
            message.at("result").at("thread").value("name", "") != model.title)
            throw std::runtime_error("Generated title was not saved by Codex");
        client.send({{"id", 9000},
                     {"method", "thread/archive"},
                     {"params", {{"threadId", model.thread_id}}}});
        return false;
    }
    if (message.value("id", nlohmann::json()) == 9000) {
        if (message.contains("error"))
            throw std::runtime_error("Could not archive the isolated test conversation");
        std::cout << "Fresh contextual message acknowledged, agent replied CANVAS_SEND_OK, "
                     "automatic title persisted, and isolated test conversation archived.\n";
        return true;
    }
    return false;
}
void test_contextual_send(const std::string& root)
{
    Conversation model(root);
    model.fresh = true;
    model.canvas_context = "Isolated canvas transport test. No project files are involved.";
    AgentClient client(TEST_PYTHON, TEST_BRIDGE);
    bool submitted = false, acknowledged = false, checking_title = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    while (std::chrono::steady_clock::now() < deadline) {
        pollfd fd{client.event_fd(), POLLIN, 0};
        poll(&fd, 1, 100);
        for (const auto& message : client.take_messages()) {
            if (handle_send_test_response(message, client, model))
                return;
            model.receive(message);
        }
        if (!model.error.empty())
            throw std::runtime_error(model.error);
        if (!model.questions.empty())
            throw std::runtime_error("The isolated send test unexpectedly requested an action");
        if (!submitted && model.ready)
            submitted = model.submit("Connection test. Do not use tools or change files. Reply "
                                     "exactly CANVAS_SEND_OK.");
        acknowledged = acknowledged || !model.accepted_text.empty();
        for (auto& message : model.outgoing())
            client.send(std::move(message));
        if (acknowledged && has_test_reply(model) && model.active_turn.empty() &&
            model.title != "Conversation" && !checking_title) {
            client.send({{"id", 8999},
                         {"method", "thread/read"},
                         {"params", {{"threadId", model.thread_id}}}});
            checking_title = true;
        }
    }
    throw std::runtime_error(
        "Timed out sending a contextual message and receiving the agent reply");
}
int run_send_test()
{
    const auto root =
        std::filesystem::temp_directory_path() / ("canvas-live-send-" + std::to_string(getpid()));
    std::filesystem::create_directories(root);
    int result = 0;
    try {
        test_contextual_send(root.string());
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    std::filesystem::remove_all(root);
    return result;
}
} // namespace

int main()
{
    if (std::getenv("SOURCE_CANVAS_TEST_SEND") && !std::getenv("CI"))
        return run_send_test();
    const auto* thread = std::getenv("CODEX_THREAD_ID");
    if (!thread || std::getenv("CI"))
        return 77;
    try {
        AgentClient client(TEST_PYTHON, TEST_BRIDGE);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        while (std::chrono::steady_clock::now() < deadline) {
            pollfd fd{client.event_fd(), POLLIN, 0};
            poll(&fd, 1, 100);
            for (const auto& message : client.take_messages()) {
                if (message.contains("error"))
                    throw std::runtime_error(
                        message["error"].value("message", "Agent request failed"));
                if (message.value("method", "") == "canvas/connected")
                    client.send(
                        {{"id", 1},
                         {"method", "initialize"},
                         {"params",
                          {{"clientInfo", {{"name", "source_canvas_test"}, {"version", "0.2"}}}}}});
                if (message.value("id", 0) == 1) {
                    client.send({{"method", "initialized"}, {"params", nlohmann::json::object()}});
                    client.send({{"id", 2},
                                 {"method", "thread/read"},
                                 {"params", {{"threadId", thread}, {"includeTurns", true}}}});
                }
                if (message.value("id", 0) == 2) {
                    const auto& data = message.at("result").at("thread");
                    if (data.at("id") != thread || data.at("turns").empty())
                        throw std::runtime_error(
                            "The current conversation must retain its real history");
                    std::cout << "Read the current Codex conversation through the native transport "
                                 "without starting a model turn.\n";
                    return 0;
                }
            }
        }
        throw std::runtime_error("Timed out reading the current Codex conversation");
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

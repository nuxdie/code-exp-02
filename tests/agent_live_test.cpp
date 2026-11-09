#include "agent_client.h"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <poll.h>
#include <stdexcept>

int main()
{
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

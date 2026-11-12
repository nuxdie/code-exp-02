#pragma once

#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

// JSON-RPC transport only; the conversation model owns history and request state.
class AgentClient {
  public:
    using Json = nlohmann::json;
    AgentClient(std::string python, std::string bridge);
    ~AgentClient();
    int event_fd() const;
    void send(const Json& message);
    std::vector<Json> take_messages();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

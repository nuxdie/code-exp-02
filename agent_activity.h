#pragma once

#include "canvas.h"

#include <array>
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct ActivityLines {
    int first = -1, last = -1; // Zero based; -1 denotes the file header.
};
std::vector<ActivityLines> activity_lines(std::string_view diff);

struct ActivityTouch {
    std::string path;
    ActivityLines lines;
    std::chrono::steady_clock::time_point received;
};
struct ActivityAgent {
    std::string label;
    std::array<float, 4> color{};
    Vec2 position, from;
    std::deque<ActivityTouch> waiting;
    std::optional<ActivityTouch> touch;
    std::optional<CanvasRect> bounds;
    std::chrono::steady_clock::time_point began{}, finished{};
    double travel = 0;
    bool arrived = false;
};
struct ActivityPulse {
    ActivityTouch touch;
    std::array<float, 4> color;
    std::chrono::steady_clock::time_point began;
};

// Only live, successful file-change notifications create actors; history is never replayed.
class AgentActivity {
  public:
    using Clock = std::chrono::steady_clock;
    using Resolve = std::function<std::optional<CanvasRect>(const ActivityTouch&)>;
    std::map<std::string, ActivityAgent> agents;
    std::vector<ActivityPulse> pulses;
    bool receive(const nlohmann::json& message, const std::string& root, const std::string& thread,
                 const std::string& label, Vec2 origin, Clock::time_point now);
    void advance(Clock::time_point now, const Resolve& resolve);
    bool active() const { return !agents.empty() || !pulses.empty(); }

  private:
    std::deque<std::string> seen;
    void advance_agent(ActivityAgent& agent, Clock::time_point now, const Resolve& resolve);
};

#include "agent_activity.h"

#include <charconv>
#include <cmath>
#include <filesystem>
#include <sstream>

namespace {
double seconds(AgentActivity::Clock::time_point now, AgentActivity::Clock::time_point then)
{
    return std::chrono::duration<double>(now - then).count();
}

std::string relative_path(const std::string& root, const std::string& path)
{
    if (path.empty())
        return {};
    auto relative = std::filesystem::path(path).lexically_normal();
    if (relative.is_absolute())
        relative = relative.lexically_relative(std::filesystem::path(root).lexically_normal());
    if (relative.empty() || *relative.begin() == ".." || relative == ".")
        return {};
    return relative.generic_string();
}

std::array<float, 4> agent_color(const std::string& thread)
{
    constexpr std::array<std::array<float, 4>, 6> palette{{{0.09f, 0.58f, 0.65f, 1},
                                                           {0.61f, 0.35f, 0.85f, 1},
                                                           {0.88f, 0.43f, 0.17f, 1},
                                                           {0.18f, 0.65f, 0.39f, 1},
                                                           {0.85f, 0.30f, 0.53f, 1},
                                                           {0.24f, 0.48f, 0.89f, 1}}};
    unsigned hash = 2166136261u;
    for (unsigned char byte : thread)
        hash = (hash ^ byte) * 16777619u;
    return palette[hash % palette.size()];
}

std::vector<ActivityTouch> item_touches(const nlohmann::json& item, const std::string& root,
                                        AgentActivity::Clock::time_point now)
{
    std::vector<ActivityTouch> touches;
    for (const auto& change : item.value("changes", nlohmann::json::array())) {
        const auto kind = change.value("kind", nlohmann::json::object());
        const auto moved = kind.value("move_path", nlohmann::json());
        const auto path = relative_path(root, moved.is_string() ? moved.get<std::string>()
                                                                : change.value("path", ""));
        if (path.empty())
            continue;
        const auto ranges = kind.value("type", "") == "delete"
                                ? std::vector<ActivityLines>{{}}
                                : activity_lines(change.value("diff", ""));
        for (const auto& lines : ranges)
            touches.push_back({path, lines, now});
    }
    return touches;
}
} // namespace

std::vector<ActivityLines> activity_lines(const std::string& diff)
{
    std::vector<ActivityLines> result;
    std::istringstream input(diff);
    std::string line;
    int row = -1;
    std::optional<ActivityLines> changed;
    auto flush = [&] {
        if (changed)
            result.push_back(*changed);
        changed.reset();
    };
    while (std::getline(input, line)) {
        if (line.starts_with("@@ ")) {
            flush();
            const auto start = line.find('+');
            int number = 0;
            row = -1;
            if (start != std::string::npos &&
                std::from_chars(line.data() + start + 1, line.data() + line.size(), number).ec ==
                    std::errc{} &&
                number >= 0)
                row = std::max(0, number - 1);
        } else if (row >= 0 && !line.empty()) {
            if (line.front() == '+' || line.front() == '-') {
                if (!changed)
                    changed = ActivityLines{row, row};
                changed->last = std::max(changed->last, row);
                if (line.front() == '+')
                    ++row;
            } else if (line.front() == ' ') {
                flush();
                ++row;
            }
        }
    }
    flush();
    if (result.empty())
        result.push_back({});
    return result;
}

bool AgentActivity::receive(const nlohmann::json& message, const std::string& root,
                            const std::string& thread, const std::string& label, Vec2 origin,
                            Clock::time_point now)
{
    if (thread.empty() || message.value("method", "") != "item/completed")
        return false;
    const auto params = message.value("params", nlohmann::json::object());
    if (params.value("threadId", "") != thread)
        return false;
    const auto item = params.value("item", nlohmann::json::object());
    if (item.value("type", "") != "fileChange" || item.value("status", "") != "completed")
        return false;
    const auto id = item.value("id", "");
    const auto key = thread + ":" + id;
    if (id.empty() || std::ranges::find(seen, key) != seen.end())
        return false;
    seen.push_back(key);
    if (seen.size() > 512)
        seen.pop_front();
    const auto touches = item_touches(item, root, now);
    if (touches.empty())
        return false;
    auto [found, inserted] = agents.try_emplace(thread);
    auto& agent = found->second;
    if (inserted) {
        agent.position = origin;
        agent.color = agent_color(thread);
    }
    agent.label = label.empty() || label == "Conversation" ? "Agent" : label;
    for (const auto& touch : touches) {
        if (agent.waiting.size() >= 32)
            agent.waiting.pop_front();
        agent.waiting.push_back(touch);
    }
    return true;
}

void AgentActivity::advance_agent(ActivityAgent& agent, Clock::time_point now,
                                  const Resolve& resolve)
{
    while (!agent.touch && !agent.waiting.empty()) {
        const auto& next = agent.waiting.front();
        const auto bounds = resolve(next);
        if (seconds(now, next.received) > 8 || (!bounds && seconds(now, next.received) > 2)) {
            agent.waiting.pop_front();
            continue;
        }
        if (!bounds)
            return; // File creation can arrive before the watcher updates the panels.
        agent.touch = next;
        agent.waiting.pop_front();
        agent.from = agent.position;
        agent.began = now;
        agent.arrived = false;
        const Vec2 target{bounds->origin.x - 46, bounds->origin.y + bounds->size.y / 2};
        agent.travel = std::clamp(
            std::hypot(target.x - agent.from.x, target.y - agent.from.y) / 1800.0, 0.3, 1.1);
    }
    if (!agent.touch)
        return;
    agent.bounds = resolve(*agent.touch);
    if (!agent.bounds) {
        agent.touch.reset();
        agent.finished = now;
        return;
    }
    const auto& bounds = *agent.bounds;
    const Vec2 target{bounds.origin.x - 46, bounds.origin.y + bounds.size.y / 2};
    const double elapsed = seconds(now, agent.began);
    const double progress = std::clamp(elapsed / agent.travel, 0.0, 1.0);
    const double eased = progress * progress * (3 - 2 * progress);
    agent.position = {agent.from.x + (target.x - agent.from.x) * eased,
                      agent.from.y + (target.y - agent.from.y) * eased};
    if (progress >= 1 && !agent.arrived) {
        pulses.push_back({*agent.touch, agent.color, now});
        agent.arrived = true;
    }
    if (elapsed >= agent.travel + 0.65) {
        agent.touch.reset();
        agent.bounds.reset();
        agent.finished = now;
    }
}

void AgentActivity::advance(Clock::time_point now, const Resolve& resolve)
{
    for (auto it = agents.begin(); it != agents.end();) {
        advance_agent(it->second, now, resolve);
        const auto& agent = it->second;
        if (!agent.touch && agent.waiting.empty() && seconds(now, agent.finished) > 1.4)
            it = agents.erase(it);
        else
            ++it;
    }
    std::erase_if(pulses, [&](const auto& pulse) { return seconds(now, pulse.began) > 2.4; });
    if (pulses.size() > 128)
        pulses.erase(pulses.begin(), pulses.end() - 128);
}

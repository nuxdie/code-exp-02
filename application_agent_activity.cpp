#include "application.h"

namespace {
std::array<float, 4> opacity(std::array<float, 4> color, double alpha)
{
    color[3] = static_cast<float>(std::clamp(alpha, 0.0, 1.0));
    return color;
}
GpuPath ring(Vec2 center, double radius, std::array<float, 4> color, float width)
{
    GpuPath path{{}, color, width, false};
    for (int i = 0; i <= 24; ++i) {
        const double angle = i * 6.283185307179586 / 24;
        path.points.push_back(
            {center.x + std::cos(angle) * radius, center.y + std::sin(angle) * radius});
    }
    return path;
}
double age(AgentActivity::Clock::time_point now, AgentActivity::Clock::time_point then)
{
    return std::chrono::duration<double>(now - then).count();
}
} // namespace

void Application::receive_agent_activity(const lsp::Json& message)
{
    const auto& card = *chat;
    const Vec2 origin{card.bounds.origin.x + card.bounds.size.x / 2, card.bounds.origin.y + 32};
    if (agent_activity.receive(message, workspace_root, card.conversation.thread_id,
                               card.conversation.title, origin, Clock::now()))
        redraw = true;
}

std::optional<CanvasRect> Application::activity_bounds(const ActivityTouch& touch)
{
    const auto found = std::ranges::find(panels, touch.path, &SourcePanel::path);
    if (found == panels.end()) {
        const auto removed = activity_deleted.find(touch.path);
        if (removed == activity_deleted.end())
            return {};
        return CanvasRect{removed->second.first.origin,
                          {removed->second.first.size.x, static_cast<double>(header_height)}};
    }
    const auto& panel = *found;
    if (touch.lines.first < 0 || panel.folded.display_lines.empty())
        return CanvasRect{panel.bounds.origin, {panel.bounds.size.x, double(header_height)}};
    const int count = static_cast<int>(panel.folded.display_lines.size());
    const int first = panel.folded.display_lines[std::clamp(touch.lines.first, 0, count - 1)];
    const int last = panel.folded.display_lines[std::clamp(touch.lines.last, 0, count - 1)];
    return CanvasRect{{panel.bounds.origin.x, panel.bounds.origin.y + header_height +
                                                  first * static_cast<double>(line_height)},
                      {panel.bounds.size.x, (last - first + 1) * static_cast<double>(line_height)}};
}

void Application::advance_agent_activity()
{
    const auto now = Clock::now();
    agent_activity.advance(now, [&](const auto& touch) { return activity_bounds(touch); });
    for (auto it = activity_deleted.begin(); it != activity_deleted.end();) {
        if (age(now, it->second.second) > 12)
            it = activity_deleted.erase(it);
        else
            ++it;
    }
    for (auto it = activity_labels.begin(); it != activity_labels.end();) {
        if (!agent_activity.agents.contains(it->first)) {
            renderer->release(it->second.mesh);
            it = activity_labels.erase(it);
        } else
            ++it;
    }
    activity_animation_due =
        agent_activity.active() ? now + std::chrono::milliseconds(16) : Clock::time_point::max();
}

void Application::draw_activity_highlights()
{
    const auto now = Clock::now();
    for (const auto& pulse : agent_activity.pulses) {
        const auto bounds = activity_bounds(pulse.touch);
        if (!bounds)
            continue;
        const auto point = camera.world_to_screen(bounds->origin, viewport());
        const Vec2 size{bounds->size.x * camera.zoom, std::max(2.0, bounds->size.y * camera.zoom)};
        const double fade = std::max(0.0, 1 - age(now, pulse.began) / 2.4);
        renderer->rectangle({point, size}, opacity(pulse.color, 0.18 * fade));
        renderer->rectangle({point, {std::max(3.0, 3 * camera.zoom), size.y}},
                            opacity(pulse.color, 0.8 * fade));
    }
}

void Application::draw_agent_activity()
{
    const auto now = Clock::now();
    std::vector<GpuPath> paths;
    for (const auto& [id, agent] : agent_activity.agents) {
        const auto point = camera.world_to_screen(agent.position, viewport());
        const bool idle = !agent.touch && agent.waiting.empty();
        const double fade = idle ? std::clamp(1 - age(now, agent.finished) / 1.4, 0.0, 1.0) : 1;
        const auto color = opacity(agent.color, fade);
        if (agent.touch && agent.bounds) {
            const auto& bounds = *agent.bounds;
            const auto target = camera.world_to_screen(
                {bounds.origin.x + 24, bounds.origin.y + bounds.size.y / 2}, viewport());
            paths.push_back({{point, target},
                             opacity(color, (agent.arrived ? 0.8 : 0.25) * fade),
                             agent.arrived ? 2.0f : 1.0f,
                             false});
            if (agent.arrived) {
                const double phase = std::clamp(age(now, agent.began) - agent.travel, 0.0, 0.65);
                paths.push_back(
                    ring(target, 4 + phase * 22, opacity(color, (1 - phase / 0.65) * fade), 1.5f));
            } else {
                const auto from = camera.world_to_screen(agent.from, viewport());
                const Vec2 tail{point.x + (from.x - point.x) * 0.14,
                                point.y + (from.y - point.y) * 0.14};
                paths.push_back({{tail, point}, opacity(color, 0.35 * fade), 3, false});
            }
        }
        paths.push_back(ring(point, 14, opacity(color, 0.15 * fade), 7));
        paths.push_back(ring({point.x, point.y - 5}, 4, color, 3));
        paths.push_back({{{point.x - 6, point.y + 7},
                          {point.x - 4, point.y + 3},
                          {point.x + 4, point.y + 3},
                          {point.x + 6, point.y + 7}},
                         color,
                         3,
                         false});
        auto& label = activity_labels[id];
        const auto title = agent.label == "Agent" ? "Agent" : "Agent · " + agent.label;
        if (label.source != title) {
            label.source = title;
            label.layout = make_line(title, {});
            pango_layout_set_width(label.layout.get(), 210 * PANGO_SCALE);
            pango_layout_set_ellipsize(label.layout.get(), PANGO_ELLIPSIZE_END);
            renderer->upload(label.mesh, {{label.layout.get(), {0, 0}}});
            int label_width = 0, label_height = 0;
            pango_layout_get_pixel_size(label.layout.get(), &label_width, &label_height);
            label.size = {double(label_width), double(label_height)};
        }
        if (fade > 0.15) {
            const Vec2 origin{point.x - label.size.x - 20, point.y - label.size.y / 2};
            renderer->rectangle(
                {{origin.x - 4, origin.y - 2}, {label.size.x + 8, label.size.y + 4}},
                {1, 1, 1, 0.92f});
            renderer->draw(label.mesh, origin, 1);
        }
    }
    renderer->paths(activity_mesh, paths);
    renderer->draw(activity_mesh, {0, 0}, 1);
    draw_agent_markers();
}

#include "application.h"

namespace {
constexpr std::array<float, 4> uses_color{0.20f, 0.45f, 0.85f, 0.75f};
constexpr std::array<float, 4> used_by_color{0.85f, 0.50f, 0.15f, 0.75f};
constexpr double edge_margin = 10;
} // namespace

// Where an end of a link is on screen: the word itself, or the file's title for a whole file.
CanvasRect Application::connection_box(const CanvasRect& word, int file)
{
    if (file >= 0 && file < static_cast<int>(header_hits.size()) && header_hits[file].size.x > 0)
        return header_hits[file];
    return {camera.world_to_screen(word.origin, viewport()),
            {word.size.x * camera.zoom, word.size.y * camera.zoom}};
}

// An arrow from one box's border to another's, so it is clear which word it belongs to.
void Application::connect_boxes(std::vector<GpuPath>& paths, const CanvasRect& from,
                                const CanvasRect& to, std::array<float, 4> color, float width)
{
    auto path = connection_curve(border_point(from, center(to)), border_point(to, center(from)));
    path.color = color;
    path.width = width;
    paths.push_back(std::move(path));
}

// Place every link for the current camera. Every word an arrow starts from is highlighted.
// A far end that is visible gets an arrow to that word. Far ends that are not are gathered
// per file into one labelled marker on the window edge in that file's direction, and the
// arrows go to the marker, so nothing ever just runs off the screen.
void Application::layout_connections()
{
    const CanvasRect view{
        {edge_margin, edge_margin},
        {std::max(1.0, width - 2 * edge_margin), std::max(1.0, height - 2 * edge_margin)}};
    connection_markers.clear();
    connection_words.clear();
    std::vector<GpuPath> paths;
    std::map<std::pair<bool, std::string>, std::vector<std::size_t>> hidden;
    for (std::size_t i = 0; i < connection_links.size(); ++i) {
        const auto& link = connection_links[i];
        const auto color = link.uses ? uses_color : used_by_color;
        const auto far = connection_box(link.target, link.far_file);
        const bool visible = inside(view, center(far)) && far.size.x > 0;
        if (visible)
            connection_words.push_back({far, color, link.far_file >= 0});
        else
            hidden[{link.uses, link.uri}].push_back(i);
        for (const auto& site : link.sites) {
            const auto near = connection_box(site, link.near_file);
            if (!inside(view, center(near)))
                continue;
            connection_words.push_back({near, color, link.near_file >= 0});
            if (visible)
                connect_boxes(paths, near, far, color, 1.5f);
        }
    }
    for (const auto& [key, members] : hidden) {
        const auto far = camera.world_to_screen(connection_links[members.front()].far, viewport());
        const auto text = marker_text(connection_links, members, direction_glyph(view, far));
        auto& label = connection_labels[text];
        update_document(label, text, 640, false);
        connection_markers.push_back({members,
                                      marker_box(view, view_exit(view, center(view), far),
                                                 {label.size.x + 22, label.size.y + 8}),
                                      text});
    }
    separate_markers(view, connection_markers);
    marker_connections(paths, view);
    renderer->paths(connection_mesh, paths);
    connection_camera = camera;
    connection_size = {static_cast<double>(width), static_cast<double>(height)};
}

// Each word is tied to the marker that stands for where it leads. The marker under the
// pointer has its arrows drawn strongly and the rest step back.
void Application::marker_connections(std::vector<GpuPath>& paths, const CanvasRect& view)
{
    const auto* pointed = connection_marker_at(mouse_position);
    connection_pointed = pointed ? static_cast<int>(pointed - connection_markers.data()) : -1;
    for (const auto& marker : connection_markers) {
        const float strength = &marker == pointed ? 0.95f : pointed ? 0.12f : 0.5f;
        for (const auto index : marker.links) {
            const auto& link = connection_links[index];
            auto color = link.uses ? uses_color : used_by_color;
            color[3] = strength;
            for (const auto& site : link.sites) {
                const auto near = connection_box(site, link.near_file);
                if (inside(view, center(near)))
                    connect_boxes(paths, near, marker.bounds, color, &marker == pointed ? 2 : 1);
            }
        }
    }
}

void Application::draw_connections(const Viewport&)
{
    if (!show_connections)
        return;
    const auto now = Clock::now();
    const auto focus = connection_focus_at(mouse_position);
    if (!focus || *focus == connection_focus) {
        connection_focus_due = Clock::time_point::max();
    } else if (!(*focus == connection_candidate) ||
               connection_focus_due == Clock::time_point::max()) {
        // Passing over other code on the way to a marker must not change the subject.
        connection_candidate = *focus;
        connection_focus_due = now + std::chrono::milliseconds(connection_links.empty() ? 0
                                                               : focus->panel < 0       ? 700
                                                                                        : 220);
    }
    if (connection_focus_due <= now) {
        connection_focus = connection_candidate;
        connection_focus_due = Clock::time_point::max();
        connections_dirty = true;
    }
    const bool moved = connection_camera.zoom != camera.zoom ||
                       connection_camera.center.x != camera.center.x ||
                       connection_camera.center.y != camera.center.y ||
                       connection_size.x != width || connection_size.y != height;
    const auto* marker = connection_marker_at(mouse_position);
    const int pointed = marker ? static_cast<int>(marker - connection_markers.data()) : -1;
    if (connections_dirty)
        rebuild_connections();
    else if (moved || pointed != connection_pointed)
        layout_connections();
    // Words are tinted beneath the text; file titles are underlined later, above their cards.
    for (const auto& word : connection_words)
        if (!word.title)
            renderer->rectangle(word.bounds,
                                {0.55f + word.color[0] * 0.45f, 0.55f + word.color[1] * 0.45f,
                                 0.55f + word.color[2] * 0.45f, 1});
    renderer->draw(connection_mesh, {0, 0}, 1);
}

void Application::draw_connection_markers()
{
    if (!show_connections)
        return;
    for (const auto& word : connection_words)
        if (word.title)
            renderer->rectangle({{word.bounds.origin.x, word.bounds.origin.y + word.bounds.size.y},
                                 {word.bounds.size.x, 2}},
                                {word.color[0], word.color[1], word.color[2], 1});
    for (const auto& marker : connection_markers) {
        const auto& color =
            connection_links[marker.links.front()].uses ? uses_color : used_by_color;
        renderer->rectangle(marker.bounds, {1, 1, 1, 0.96f});
        renderer->rectangle({marker.bounds.origin, {3, marker.bounds.size.y}},
                            {color[0], color[1], color[2], 1});
        renderer->draw(connection_labels[marker.text].mesh,
                       {marker.bounds.origin.x + 12, marker.bounds.origin.y + 4}, 1);
    }
    // Labels are cached by their text; forget the ones no longer shown.
    for (auto label = connection_labels.begin(); label != connection_labels.end();) {
        const bool shown =
            std::any_of(connection_markers.begin(), connection_markers.end(),
                        [&](const auto& marker) { return marker.text == label->first; });
        if (shown) {
            ++label;
            continue;
        }
        renderer->release(label->second.mesh);
        label = connection_labels.erase(label);
    }
}

const ConnectionMarker* Application::connection_marker_at(Vec2 pointer)
{
    if (!show_connections)
        return nullptr;
    for (const auto& marker : connection_markers)
        if (CanvasWindowManager::contains(marker.bounds, pointer))
            return &marker;
    return nullptr;
}

// A marker is a way to get there.
bool Application::connection_marker_button(const XEvent& event, Vec2 pointer)
{
    if (!drag_button && event.xbutton.button == Button1)
        for (const auto& agent : agent_markers)
            if (CanvasWindowManager::contains(agent.bounds, pointer)) {
                const lsp::Json at{{"line", agent.line}, {"character", 0}};
                return jump_to({{"uri", agent.uri}, {"range", {{"start", at}, {"end", at}}}});
            }
    const auto* marker = connection_marker_at(pointer);
    if (!marker || drag_button || event.xbutton.button != Button1)
        return false;
    const auto link = connection_links[marker->links.front()];
    const lsp::Json position{{"line", link.line}, {"character", link.column}};
    return jump_to({{"uri", link.uri}, {"range", {{"start", position}, {"end", position}}}});
}

// An agent editing out of sight is announced on the window edge in its direction, with the
// file it is in; a click flies there.
void Application::draw_agent_markers()
{
    const CanvasRect view{
        {edge_margin, edge_margin},
        {std::max(1.0, width - 2 * edge_margin), std::max(1.0, height - 2 * edge_margin)}};
    agent_markers.clear();
    std::vector<ConnectionMarker> boxes;
    std::vector<std::string> shown;
    for (const auto& [id, agent] : agent_activity.agents) {
        const auto panel = std::find_if(panels.begin(), panels.end(), [&](const auto& source) {
            return agent.touch && source.path == agent.touch->path;
        });
        if (!agent.bounds || panel == panels.end())
            continue;
        const auto target = camera.world_to_screen(
            {agent.bounds->origin.x + 24, agent.bounds->origin.y + agent.bounds->size.y / 2},
            viewport());
        if (inside(view, target))
            continue;
        const auto text = std::string(direction_glyph(view, target)) + " " +
                          std::filesystem::path(panel->path).filename().string() + "  " +
                          agent.label;
        auto& label = agent_marker_labels[id];
        update_document(label, text, 520, false);
        boxes.push_back({{agent_markers.size()},
                         marker_box(view, view_exit(view, {width / 2.0, height / 2.0}, target),
                                    {label.size.x + 22, label.size.y + 8}),
                         id});
        agent_markers.push_back({{}, panel->uri, std::max(0, agent.touch->lines.first)});
        shown.push_back(id);
    }
    separate_markers(view, boxes);
    for (const auto& box : boxes) {
        const auto& color = agent_activity.agents.at(box.text).color;
        agent_markers[box.links.front()].bounds = box.bounds;
        renderer->rectangle(box.bounds, {1, 1, 1, 0.96f});
        renderer->rectangle({box.bounds.origin, {3, box.bounds.size.y}},
                            {color[0], color[1], color[2], 1});
        renderer->draw(agent_marker_labels[box.text].mesh,
                       {box.bounds.origin.x + 12, box.bounds.origin.y + 4}, 1);
    }
    for (auto label = agent_marker_labels.begin(); label != agent_marker_labels.end();) {
        if (std::find(shown.begin(), shown.end(), label->first) != shown.end()) {
            ++label;
            continue;
        }
        renderer->release(label->second.mesh);
        label = agent_marker_labels.erase(label);
    }
}

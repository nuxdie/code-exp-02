#include "application.h"

namespace {
constexpr std::array<float, 4> uses_color{0.20f, 0.45f, 0.85f, 0.75f};
constexpr std::array<float, 4> used_by_color{0.85f, 0.50f, 0.15f, 0.75f};
constexpr std::array<float, 4> subject_color{0.55f, 0.45f, 0.10f, 0.9f};
constexpr double edge_margin = 10;
} // namespace

// Where a word is on screen right now, or nothing when its file is not on the canvas.
CanvasRect Application::connection_box(const ConnectionEnd& end)
{
    const auto panel = std::ranges::find(panels, end.uri, &SourcePanel::uri);
    if (panel == panels.end() || end.line >= std::ssize(panel->text))
        return {};
    const auto word = word_bounds(*panel, end.line, end.begin, end.end);
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

// A labelled marker on the window edge, in the direction of a word that is out of view.
void Application::mark_connection(const CanvasRect& view, std::vector<std::size_t> members,
                                  const ConnectionEnd& end, std::vector<MarkerPart> parts)
{
    const auto far = center(connection_box(end));
    parts.front().text = std::string(direction_glyph(view, far)) + " " + parts.front().text;
    // Each part is its own piece of text, so each can be pointed at.
    double right = 12, tall = 0;
    std::string text;
    for (auto& part : parts) {
        auto& label = connection_labels[part.text];
        update_document(label, part.text, 640, false);
        part.offset = right;
        part.width = label.size.x;
        right += label.size.x + (&part == &parts.front() ? 14 : 8);
        tall = std::max(tall, label.size.y);
        text += part.text + " ";
    }
    connection_markers.push_back(
        {std::move(members),
         marker_box(view, view_exit(view, center(view), far), {right + 2, tall + 8}),
         std::move(text), std::move(parts)});
}

// Sort the links into words that are in view, highlighted, and markers for the rest.
std::vector<std::size_t> Application::place_connection_ends(const CanvasRect& view)
{
    std::map<std::pair<bool, std::string>, std::vector<std::size_t>> hidden;
    std::vector<std::size_t> visible;
    for (std::size_t i = 0; i < connection_links.size(); ++i) {
        const auto& link = connection_links[i];
        const auto box = connection_box(link.at);
        if (box.size.x > 0 && inside(view, center(box))) {
            connection_words.push_back({box, link.defines ? uses_color : used_by_color});
            visible.push_back(i);
        } else {
            hidden[{link.defines, link.at.uri}].push_back(i);
        }
    }
    for (const auto& [key, members] : hidden)
        mark_connection(view, members, connection_links[members.front()].at,
                        marker_parts(connection_links, members));
    return visible;
}

// Arrows between the subject and each end. The marker under the pointer stands out.
void Application::connection_arrows(std::vector<GpuPath>& paths, const CanvasRect& subject,
                                    const std::vector<std::size_t>& visible, bool to_markers)
{
    const auto* pointed = connection_marker_at(mouse_position);
    connection_pointed = pointed ? static_cast<int>(pointed - connection_markers.data()) : -1;
    const auto join = [&](const CanvasRect& other, bool defines, float strength, float width) {
        auto color = defines ? uses_color : used_by_color;
        color[3] = strength;
        connect_boxes(paths, defines ? subject : other, defines ? other : subject, color, width);
    };
    const float rest = pointed ? 0.2f : 0.8f;
    for (const auto index : visible)
        join(connection_box(connection_links[index].at), connection_links[index].defines, rest,
             1.5f);
    for (const auto& marker : connection_markers)
        if (!marker.links.empty() && to_markers)
            join(marker.bounds, connection_links[marker.links.front()].defines,
                 &marker == pointed ? 0.95f : rest, &marker == pointed ? 2.5f : 1.5f);
}

// Place the subject's links for the current camera. Arrows run from the subject to where it
// is defined, and from each use to the subject. An end that is in view is a highlighted
// word. Ends that are not are gathered per file into one labelled marker on the window
// edge in that file's direction, the subject included, so no arrow runs off the screen.
void Application::layout_connections()
{
    const CanvasRect view{
        {edge_margin, edge_margin},
        {std::max(1.0, width - 2 * edge_margin), std::max(1.0, height - 2 * edge_margin)}};
    connection_markers.clear();
    connection_words.clear();
    connection_camera = camera;
    connection_size = {static_cast<double>(width), static_cast<double>(height)};
    connection_subject_box = {};
    std::vector<GpuPath> paths;
    if (!connection_subject.active) {
        renderer->paths(connection_mesh, paths);
        return;
    }
    auto subject = connection_box(connection_subject.at);
    const bool subject_visible = inside(view, center(subject));
    const auto visible = place_connection_ends(view);
    if (subject_visible) {
        connection_subject_box = subject;
        connection_words.push_back({subject, subject_color});
    } else if (!visible.empty()) {
        mark_connection(view, {}, connection_subject.at,
                        {{connection_subject.file + "  " + connection_subject.name}});
    }
    separate_markers(view, connection_markers);
    // Arrows meet the subject itself, or the marker standing for it while it is out of view.
    for (const auto& marker : connection_markers)
        if (marker.links.empty())
            subject = marker.bounds;
    connection_arrows(paths, subject, visible, subject_visible);
    renderer->paths(connection_mesh, paths);
}

void Application::draw_connections(const Viewport&)
{
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
    // Words are tinted beneath the text so the code stays legible.
    for (const auto& word : connection_words)
        renderer->rectangle(word.bounds,
                            {0.55f + word.color[0] * 0.45f, 0.55f + word.color[1] * 0.45f,
                             0.55f + word.color[2] * 0.45f, 1});
    renderer->draw(connection_mesh, {0, 0}, 1);
}

void Application::draw_connection_markers()
{
    // The subject is underlined above the text, so it reads even on a highlighted line.
    const auto& chosen = connection_subject_box;
    if (chosen.size.x > 0)
        renderer->rectangle({{chosen.origin.x, chosen.origin.y + chosen.size.y - 2},
                             {chosen.size.x, std::max(2.0, 2 * camera.zoom)}},
                            {0.95f, 0.75f, 0.1f, 1});
    for (const auto& marker : connection_markers) {
        const auto& color = marker.links.empty()                             ? subject_color
                            : connection_links[marker.links.front()].defines ? uses_color
                                                                             : used_by_color;
        renderer->rectangle(marker.bounds, {1, 1, 1, 0.96f});
        renderer->rectangle({marker.bounds.origin, {3, marker.bounds.size.y}},
                            {color[0], color[1], color[2], 1});
        for (const auto& part : marker.parts)
            renderer->draw(connection_labels[part.text].mesh,
                           {marker.bounds.origin.x + part.offset, marker.bounds.origin.y + 4}, 1);
        // The word a click would fly to.
        if (const auto* part = marker_part_at(marker, mouse_position))
            renderer->rectangle({{marker.bounds.origin.x + part->offset,
                                  marker.bounds.origin.y + marker.bounds.size.y - 4},
                                 {part->width, 1.5}},
                                {color[0], color[1], color[2], 1});
    }
    // Labels are cached by their text; forget the ones no longer shown.
    for (auto label = connection_labels.begin(); label != connection_labels.end();) {
        const bool shown = std::ranges::any_of(connection_markers, [&](const auto& marker) {
            return std::ranges::any_of(marker.parts,
                                       [&](const auto& part) { return part.text == label->first; });
        });
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
    // The word under the pointer decides where to go; elsewhere on the marker, its first.
    const auto* part = marker_part_at(*marker, pointer);
    const auto at = marker->links.empty()
                        ? connection_subject.at
                        : connection_links[(part ? *part : marker->parts.front()).link].at;
    const auto& text = std::ranges::find(panels, at.uri, &SourcePanel::uri)->text[at.line];
    const lsp::Json position{{"line", at.line}, {"character", lsp::byte_to_utf16(text, at.begin)}};
    return jump_to({{"uri", at.uri}, {"range", {{"start", position}, {"end", position}}}});
}

bool Application::connection_key(const XEvent& event)
{
    auto key = event.xkey;
    return XLookupKeysym(&key, 0) == XK_Escape && popup_text.empty() && !drag_button &&
           clear_connections();
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
        const auto panel = std::ranges::find_if(panels, [&](const auto& source) {
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
        boxes.push_back(
            {.links = {agent_markers.size()},
             .bounds = marker_box(view, view_exit(view, {width / 2.0, height / 2.0}, target),
                                  {label.size.x + 22, label.size.y + 8}),
             .text = id,
             .parts = {}});
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
        if (std::ranges::find(shown, label->first) != shown.end()) {
            ++label;
            continue;
        }
        renderer->release(label->second.mesh);
        label = agent_marker_labels.erase(label);
    }
}

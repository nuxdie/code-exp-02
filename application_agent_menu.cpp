#include "application.h"

void Application::open_chat_input(Vec2 pointer)
{
    auto context = agent_context(pointer);
    auto position = camera.screen_to_world(pointer, viewport());
    const auto panel = std::find_if(panels.begin(), panels.end(), [&](const auto& source) {
        return source.path == context.value("path", "");
    });
    if (panel != panels.end() && !panel->folded.display_lines.empty() &&
        Viewport{panel->bounds.origin.x, panel->bounds.origin.y, panel->bounds.size.x,
                 panel->bounds.size.y}
            .contains(position)) {
        int line = context.value("line", 1) - 1;
        if (context.contains("selection_range")) {
            const auto end = context["selection_range"].value("end", std::size_t{0});
            line = static_cast<int>(std::count(
                panel->source.begin(),
                panel->source.begin() + std::min(end ? end - 1 : 0, panel->source.size()), '\n'));
        }
        line = std::clamp(line, 0, static_cast<int>(panel->folded.display_lines.size()) - 1);
        position.y =
            std::max(position.y, panel->bounds.origin.y + header_height +
                                     (panel->folded.display_lines[line] + 1) * line_height + 12);
    }
    spawn_agent_window(position, std::move(context));
}
bool Application::agent_window_key(const XEvent& event)
{
    auto key_event = event.xkey;
    const auto key = XLookupKeysym(&key_event, 0);
    if ((event.xkey.state & (ControlMask | ShiftMask)) != (ControlMask | ShiftMask) ||
        (key != XK_a && key != XK_A))
        return false;
    const auto pointer = viewport().contains(mouse_position) ? mouse_position : viewport().center();
    open_chat_input(pointer);
    return true;
}

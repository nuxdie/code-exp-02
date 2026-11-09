#include "application.h"

void Application::show_agent_menu(Vec2 pointer)
{
    agent_spawn_world = camera.screen_to_world(pointer, viewport());
    agent_spawn_context = agent_context(pointer);
    show_popup("New agent here", pointer);
    agent_menu = true;
}
bool Application::agent_window_menu(const XEvent& event, Vec2, bool in_popup)
{
    if (!agent_menu)
        return false;
    if (event.xbutton.button != Button1 || !in_popup)
        return false;
    const auto position = agent_spawn_world;
    auto context = agent_spawn_context;
    hide_popup();
    spawn_agent_window(position, std::move(context));
    return true;
}
bool Application::agent_window_key(const XEvent& event)
{
    auto key_event = event.xkey;
    const auto key = XLookupKeysym(&key_event, 0);
    if ((event.xkey.state & (ControlMask | ShiftMask)) != (ControlMask | ShiftMask) ||
        (key != XK_a && key != XK_A))
        return false;
    const auto pointer = viewport().contains(mouse_position) ? mouse_position : viewport().center();
    auto context = agent_context(pointer);
    const auto position = camera.screen_to_world(pointer, viewport());
    spawn_agent_window(position, std::move(context));
    return true;
}

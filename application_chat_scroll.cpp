#include "application.h"

void Application::draw_chat_scrollbar()
{
    const auto track = chat->scrollbar_track();
    if (track.size.y <= 0)
        return;
    const auto thumb = chat->scrollbar_thumb();
    const auto point = camera.world_to_screen(thumb.origin, viewport());
    const auto world = camera.screen_to_world(mouse_position, viewport());
    const bool hover =
        Viewport{track.origin.x, track.origin.y, track.size.x, track.size.y}.contains(world);
    renderer->rectangle(
        {{point.x + 4 * camera.zoom, point.y}, {8 * camera.zoom, thumb.size.y * camera.zoom}},
        {0.5f, 0.54f, 0.6f, chat->scrolling || hover ? 0.95f : 0.65f});
    chat->hits.push_back({ChatHit::Kind::Scrollbar, track});
}
void Application::begin_chat_scroll(Vec2 world)
{
    const auto thumb = chat->scrollbar_thumb();
    if (thumb.size.y <= 0)
        return;
    if (XGrabPointer(display, window, False,
                     ButtonPressMask | ButtonReleaseMask | PointerMotionMask, GrabModeAsync,
                     GrabModeAsync, None, normal_cursor, CurrentTime) != GrabSuccess)
        return;
    const bool on_thumb = world.y >= thumb.origin.y && world.y < thumb.origin.y + thumb.size.y;
    chat->scroll_grab_fraction = on_thumb ? (world.y - thumb.origin.y) / thumb.size.y : 0.5;
    chat->scrolling = true;
    chat->restore_anchor.clear();
    drag_button = Button1;
    dragged_panel = -1;
    last_pointer = mouse_position;
    drag_chat_scroll(world);
    update_cursor();
}
void Application::drag_chat_scroll(Vec2 world)
{
    const double maximum = std::max(0.0, chat->content_height - chat->body.size.y);
    chat->scroll = canvas_scrollbar_drag(chat->scrollbar_track(), chat->content_height,
                                         chat->scroll, world.y, chat->scroll_grab_fraction);
    chat->follow = chat->scroll >= maximum;
    chat->restore_anchor.clear();
    schedule_save();
}

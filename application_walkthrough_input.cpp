#include "application.h"

bool Application::walkthrough_event(const XEvent& event, Vec2 pointer)
{
    if (event.type == MotionNotify) {
        if (dragged_walkthrough_step < 0)
            return false;
        drag_walkthrough(pointer);
        return true;
    }
    if (!walkthrough || drag_button || search.active || chat_at(pointer))
        return false;
    const auto world = camera.screen_to_world(pointer, viewport());
    auto contains = [&](const CanvasRect& bounds) {
        return Viewport{bounds.origin.x, bounds.origin.y, bounds.size.x, bounds.size.y}.contains(
            world);
    };
    if (!contains(walkthrough->bounds))
        return false;
    if ((!popup_text.empty() && Viewport{popup_bounds.origin.x, popup_bounds.origin.y,
                                         popup_bounds.size.x, popup_bounds.size.y}
                                    .contains(pointer)) ||
        (intent && intent->visible && contains(intent->bounds)))
        return false;
    if (event.xbutton.button == Button2) {
        release_walkthrough();
        schedule_save();
        return true;
    }
    if (event.xbutton.button != Button1)
        return false;
    for (std::size_t i = 0; i < walkthrough->steps.size(); ++i) {
        const auto& step = walkthrough->steps[i];
        if (!contains(step.bounds))
            continue;
        if (world.y >= step.bounds.origin.y + step.heading.size.y + 24) {
            if (!step.available)
                return true;
            const auto row = std::max(
                0, static_cast<int>((world.y - step.bounds.origin.y - step.code_y) / line_height));
            open_link(step.path + ":" + std::to_string(std::min(step.last, step.first + row)));
        } else if (XGrabPointer(display, window, False,
                                ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                                GrabModeAsync, GrabModeAsync, None, normal_cursor,
                                CurrentTime) == GrabSuccess) {
            drag_button = Button1;
            dragged_walkthrough_step = static_cast<int>(i);
            last_pointer = pointer;
            drag_moved = false;
        }
        return true;
    }
    return true;
}

void Application::drag_walkthrough(Vec2 pointer)
{
    hide_popup();
    mouse_position = pointer;
    redraw = true;
    const Vec2 delta{pointer.x - last_pointer.x, pointer.y - last_pointer.y};
    auto& position = walkthrough->steps[dragged_walkthrough_step].bounds.origin;
    position.x = std::max(walkthrough->origin.x, position.x + delta.x / camera.zoom);
    position.y = std::max(walkthrough->origin.y + walkthrough->heading.size.y + 32,
                          position.y + delta.y / camera.zoom);
    last_pointer = zoom_animation.anchor = pointer;
}

nlohmann::json Application::save_walkthrough()
{
    if (!walkthrough)
        return nullptr;
    auto saved = walkthrough->specification;
    saved["origin"] = {{"x", walkthrough->origin.x}, {"y", walkthrough->origin.y}};
    for (std::size_t i = 0; i < walkthrough->steps.size(); ++i) {
        const auto& step = walkthrough->steps[i];
        saved["steps"][i]["x"] = step.bounds.origin.x - walkthrough->origin.x;
        saved["steps"][i]["y"] =
            step.bounds.origin.y - walkthrough->origin.y - walkthrough->heading.size.y - 32;
    }
    return saved;
}

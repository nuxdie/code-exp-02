#include "application.h"

const WalkthroughStep* Application::walkthrough_code_at(Vec2 pointer)
{
    if (!walkthrough || window_at(pointer) != WindowKind::Walkthrough)
        return nullptr;
    if (CanvasWindowManager::contains(walkthrough->header_bounds, pointer))
        return nullptr;
    for (const auto& step : walkthrough->steps)
        if (CanvasWindowManager::contains(step.header_bounds, pointer))
            return nullptr;
    const auto world = camera.screen_to_world(pointer, viewport());
    for (auto i = walkthrough->steps.rbegin(); i != walkthrough->steps.rend(); ++i) {
        const auto& step = *i;
        if (step.available &&
            Viewport{step.bounds.origin.x + 16, step.bounds.origin.y + step.code_y,
                     step.bounds.size.x - 32, (step.last - step.first + 1.0) * line_height}
                .contains(world))
            return &step;
    }
    return nullptr;
}

bool Application::walkthrough_event(const XEvent& event, Vec2 pointer)
{
    if (event.type == MotionNotify) {
        if (dragged_walkthrough_step == -1)
            return false;
        drag_walkthrough(pointer);
        return true;
    }
    if (!walkthrough || drag_button || window_at(pointer) != WindowKind::Walkthrough)
        return false;
    const auto world = camera.screen_to_world(pointer, viewport());
    auto contains = [&](const CanvasRect& bounds) {
        return Viewport{bounds.origin.x, bounds.origin.y, bounds.size.x, bounds.size.y}.contains(
            world);
    };
    if (event.xbutton.button != Button1)
        return false;
    if (CanvasWindowManager::contains(walkthrough->header_bounds, pointer)) {
        if (begin_window_drag(walkthrough->origin, pointer))
            dragged_walkthrough_step = -2;
        return true;
    }
    for (std::size_t i = walkthrough->steps.size(); i-- > 0;) {
        const auto& step = walkthrough->steps[i];
        const bool title = CanvasWindowManager::contains(step.header_bounds, pointer);
        if (!title && !contains(step.bounds))
            continue;
        if (!title && walkthrough_code_at(pointer))
            return false;
        if ((title || world.y < step.bounds.origin.y + step.heading.size.y + 24) &&
            begin_window_drag(walkthrough->steps[i].bounds.origin, pointer))
            dragged_walkthrough_step = static_cast<int>(i);
        return true;
    }
    begin_drag(event, pointer, viewport(), true);
    return true;
}

void Application::drag_walkthrough(Vec2 pointer)
{
    hide_popup();
    mouse_position = pointer;
    redraw = true;
    const Vec2 delta{pointer.x - last_pointer.x, pointer.y - last_pointer.y};
    window_manager.move(delta, camera.zoom);
    if (dragged_walkthrough_step == -2)
        for (auto& step : walkthrough->steps) {
            step.bounds.origin.x += delta.x / camera.zoom;
            step.bounds.origin.y += delta.y / camera.zoom;
        }
    last_pointer = zoom_animation.anchor = pointer;
}

nlohmann::json Application::save_walkthrough()
{
    if (!walkthrough)
        return nullptr;
    auto saved = walkthrough->specification;
    saved["origin"] = {{"x", walkthrough->origin.x}, {"y", walkthrough->origin.y}};
    for (std::size_t i = walkthrough->steps.size(); i-- > 0;) {
        const auto& step = walkthrough->steps[i];
        saved["steps"][i]["x"] = step.bounds.origin.x - walkthrough->origin.x;
        saved["steps"][i]["y"] =
            step.bounds.origin.y - walkthrough->origin.y - walkthrough->heading.size.y - 32;
    }
    return saved;
}

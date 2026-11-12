#include "application.h"

bool Application::begin_window_drag(Vec2& origin, Vec2 pointer)
{
    if (XGrabPointer(display, window, False,
                     ButtonPressMask | ButtonReleaseMask | PointerMotionMask, GrabModeAsync,
                     GrabModeAsync, None, normal_cursor, CurrentTime) != GrabSuccess)
        return false;
    window_manager.begin(origin);
    drag_button = Button1;
    drag_moved = false;
    last_pointer = zoom_animation.anchor = pointer;
    return true;
}

void Application::draw_window_title(DocumentView& title, const std::string& text, CanvasRect world,
                                    CanvasRect& hit, std::vector<CanvasRect>& occupied)
{
    const int text_width = static_cast<int>(world.size.x - 64);
    if (!title.layout || title.source != text || title.width != text_width ||
        title.generation != document_generation) {
        title.source = text;
        title.width = text_width;
        title.generation = document_generation;
        title.document = popup_document(text, false);
        title.layout = make_line(text, {{0, text.size(), Syntax::Directive}});
        pango_layout_set_width(title.layout.get(), text_width * PANGO_SCALE);
        pango_layout_set_height(title.layout.get(), -1);
        pango_layout_set_ellipsize(title.layout.get(), PANGO_ELLIPSIZE_END);
        int measured = 0;
        pango_layout_get_pixel_size(title.layout.get(), &measured, nullptr);
        title.size = {static_cast<double>(measured + 34), 27};
        renderer->upload(title.mesh, {{title.layout.get(), {32, 0}}});
    }
    hit = {};
    const auto origin = camera.world_to_screen(world.origin, viewport());
    const double scale = CanvasWindowManager::title_scale(camera.zoom);
    if (!CanvasRect{origin, {title.size.x * scale, title.size.y * scale}}.intersects(
            {{0, 0}, {static_cast<double>(width), static_cast<double>(height)}}))
        return;
    hit = CanvasWindowManager::title_bounds(origin, title.size, camera.zoom, width, occupied);
    occupied.push_back(hit);
    renderer->rectangle(hit, {0.995f, 0.993f, 0.985f, 1});
    renderer->draw(title.mesh, hit.origin, scale);
}

Application::WindowKind Application::window_at(Vec2 pointer)
{
    const auto world = camera.screen_to_world(pointer, viewport());
    const auto contains = CanvasWindowManager::contains;
    // Reverse paint order is also the input order for every canvas surface.
    if (search.active)
        return WindowKind::Search;
    if (!popup_text.empty() && contains(popup_bounds, pointer))
        return WindowKind::Popup;
    const auto hit_chat = [&](const auto& card) {
        return card && (contains(card->header_bounds, pointer) ||
                        (card->visible && contains(card->bounds, world)));
    };
    if (hit_chat(chat))
        return WindowKind::Chat;
    if (std::any_of(chat_windows.begin(), chat_windows.end(), hit_chat))
        return WindowKind::Chat;
    for (const auto& bounds : header_hits)
        if (contains(bounds, pointer))
            return WindowKind::Source;
    for (const auto& panel : panels)
        if (contains(panel.bounds, world))
            return WindowKind::Source;
    return WindowKind::Canvas;
}

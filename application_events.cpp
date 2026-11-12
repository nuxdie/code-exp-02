#include "application.h"

void Application::handle_event(const XEvent& event)
{
    if (workspace_load && event.type != Expose && event.type != ConfigureNotify &&
        event.type != ClientMessage && event.type != DestroyNotify)
        return;

    switch (event.type) {
    case Expose:
        if (!event.xexpose.count)
            redraw = true;
        break;
    case ConfigureNotify:
        width = event.xconfigure.width;
        height = event.xconfigure.height;
        redraw = true;
        break;
    case SelectionNotify:
        if (event.xselection.property ==
            XInternAtom(display, "SOURCE_CANVAS_SEARCH_PASTE", False)) {
            paste_search(event);
            break;
        }
        chat_selection(event);
        break;
    case SelectionRequest:
        chat_selection(event);
        break;
    case ButtonPress:
        handle_button(event);
        break;
    case MotionNotify:
        handle_motion(event);
        break;
    case ButtonRelease:
        if (event.xbutton.button == drag_button)
            handle_release(event);
        break;
    case KeyPress:
        handle_key(event);
        break;
    case LeaveNotify:
        if (!drag_button) {
            mouse_position = {static_cast<double>(event.xcrossing.x),
                              static_cast<double>(event.xcrossing.y)};
            popup_dismiss_due = search.active || shortcuts_visible ? Clock::time_point::max()
                                                                   : Clock::now() + 300ms;
            mouse_position = {-1, -1};
            redraw = true;
        }
        break;
    case ClientMessage:
        if (event.xclient.message_type == protocols && event.xclient.format == 32 &&
            static_cast<Atom>(event.xclient.data.l[0]) == close_message)
            running = false;
        break;
    case DestroyNotify:
        window_destroyed = true;
        running = false;
        break;
    }
}

void Application::handle_button(const XEvent& event)
{
    const Vec2 pointer{static_cast<double>(event.xbutton.x), static_cast<double>(event.xbutton.y)};
    const auto view = viewport();
    mouse_position = pointer;
    redraw = true;
    if (navigation_button(event.xbutton.button))
        return;
    if (close_window_button(event, pointer)) {
        hover_due = Clock::time_point::max();
        update_cursor();
        return;
    }
    dismiss_empty_chat(event, pointer);
    if (search_button(event, pointer))
        return;
    if (event.xbutton.button == Button3 && !drag_button) {
        open_chat_input(pointer);
        return;
    }
    if (event.xbutton.button == Button1 && !drag_button && activate_chat(pointer) &&
        reopen_chat(pointer))
        return;
    const bool in_popup =
        !popup_text.empty() && Viewport{popup_bounds.origin.x, popup_bounds.origin.y,
                                        popup_bounds.size.x, popup_bounds.size.y}
                                   .contains(pointer);
    if (connection_marker_button(event, pointer) || intent_margin_button(event, pointer) ||
        begin_text_selection(event, pointer) || popup_button(event, in_popup) ||
        chat_button(event, pointer))
        return;
    schedule_save();
    hide_popup();
    handle_canvas_button(event, pointer, view);
}

void Application::handle_canvas_button(const XEvent& event, Vec2 pointer, const Viewport& view)
{
    if (event.xbutton.button == Button1 && !(event.xbutton.state & Mod1Mask) &&
        open_document_link(pointer))
        return;
    if (event.xbutton.button == Button4 || event.xbutton.button == Button5) {
        zoom_wheel(event.xbutton.button, pointer);
    } else if (event.xbutton.button == Button1 && (event.xbutton.state & Mod1Mask)) {
        select_intent_at(pointer);
    } else if ((event.xbutton.button == Button1 || event.xbutton.button == Button2) &&
               !drag_button && view.contains(pointer)) {
        begin_drag(event, pointer, view);
    }
    redraw = true;
}

bool Application::close_window_button(const XEvent& event, Vec2 pointer)
{
    if (event.xbutton.button != Button2 || drag_button)
        return false;
    auto contains = [](const CanvasRect& bounds, Vec2 point) {
        return Viewport{bounds.origin.x, bounds.origin.y, bounds.size.x, bounds.size.y}.contains(
            point);
    };
    const auto target = window_at(pointer);
    if (target == WindowKind::Search) {
        if (!contains(search.bounds, pointer))
            return false;
        close_search();
    } else if (target == WindowKind::Popup) {
        if (text_selection.surface == TextSurface::Popup)
            text_selection = {};
        hide_popup();
    } else if (target == WindowKind::Chat && activate_chat(pointer))
        close_agent_window();
    else
        return false;
    return true;
}

void Application::zoom_wheel(unsigned button, Vec2 pointer)
{
    remember_view(true);
    const auto view = viewport();
    if (drag_button) {
        if ((!chat || !chat->scrolling) && !text_selection.dragging)
            camera.pan({pointer.x - last_pointer.x, pointer.y - last_pointer.y});
        last_pointer = pointer;
    }
    zoom_animation.request(camera, button == Button4 ? 1.15 : 1 / 1.15,
                           drag_button || view.contains(pointer) ? pointer : view.center());
}

void Application::handle_motion(const XEvent& event)
{
    const Vec2 pointer{static_cast<double>(event.xmotion.x), static_cast<double>(event.xmotion.y)};
    mouse_position = pointer;
    redraw = true;
    update_cursor();
    if ((search.active || shortcuts_visible) && !drag_button) {
        hover_due = popup_dismiss_due = Clock::time_point::max();
        return;
    }
    if (text_selection.dragging) {
        move_text_selection(pointer);
        return;
    }
    if (drag_button)
        handle_drag_motion(pointer);
    else
        update_pointer_hover(pointer);
}

void Application::handle_drag_motion(Vec2 pointer)
{
    hide_popup();
    const Vec2 delta{pointer.x - last_pointer.x, pointer.y - last_pointer.y};
    drag_moved = drag_moved || std::hypot(delta.x, delta.y) > 1;
    if (chat && chat->scrolling) {
        drag_chat_scroll(camera.screen_to_world(pointer, viewport()));
    } else if (window_manager.moving) {
        window_manager.move(delta, camera.zoom);
        if (dragged_panel >= 0) {
            const auto& bounds = panels[dragged_panel].bounds;
            content_size.x = std::max(content_size.x, bounds.origin.x + bounds.size.x);
            content_size.y = std::max(content_size.y, bounds.origin.y + bounds.size.y);
            connections_dirty = true;
        }
    } else {
        remember_view(true);
        camera.pan(delta);
    }
    last_pointer = pointer;
    zoom_animation.anchor = pointer;
    redraw = true;
}

void Application::update_pointer_hover(Vec2 pointer)
{
    const Viewport popup_view{popup_bounds.origin.x, popup_bounds.origin.y, popup_bounds.size.x,
                              popup_bounds.size.y};
    if (!popup_text.empty() && popup_view.contains(pointer)) {
        popup_dismiss_due = hover_due = Clock::time_point::max();
        return;
    }
    if (!popup_text.empty() && popup_dismiss_due == Clock::time_point::max())
        popup_dismiss_due = Clock::now() + 300ms;
    hover_due = chat_at(pointer) ? Clock::time_point::max() : Clock::now() + 400ms;
}

void Application::handle_release(const XEvent& event)
{
    if (text_selection.dragging) {
        end_text_selection(event);
        return;
    }
    if (chat && chat->scrolling)
        drag_chat_scroll(camera.screen_to_world(
            {static_cast<double>(event.xbutton.x), static_cast<double>(event.xbutton.y)},
            viewport()));
    window_manager.end();
    drag_button = 0;
    XUngrabPointer(display, CurrentTime);
    dragged_panel = -1;
    if (chat) {
        chat->dragging = false;
        chat->scrolling = false;
    }
    if (!zoom_animation.active(camera))
        finish_view();
    schedule_save();
    redraw = true;
    update_cursor();
}

bool Application::cancel_pointer_drag(const XEvent& event)
{
    auto key = event.xkey;
    if (XLookupKeysym(&key, 0) != XK_Escape ||
        (!window_manager.moving && !text_selection.dragging && (!chat || !chat->scrolling)))
        return false;
    text_selection.moved = true;
    drag_moved = true;
    XEvent release{};
    release.type = ButtonRelease;
    release.xbutton.button = drag_button;
    release.xbutton.x = static_cast<int>(mouse_position.x);
    release.xbutton.y = static_cast<int>(mouse_position.y);
    handle_release(release);
    return true;
}

void Application::handle_key(const XEvent& event)
{
    if (cancel_pointer_drag(event) || canvas_shortcut_key(event) || agent_window_key(event) ||
        selection_key(event) || chat_key(event) || intent_margin_key(event) ||
        connection_key(event)) {
        redraw = true;
        return;
    }
    auto escape_event = event.xkey;
    if (XLookupKeysym(&escape_event, 0) == XK_Escape) {
        hide_popup();
        drag_button = 0;
        XUngrabPointer(display, CurrentTime);
        redraw = true;
        return;
    }
    schedule_save();
    hide_popup();
    auto key_event = event.xkey;
    const auto key = XLookupKeysym(&key_event, 0);
    if (camera_key(key)) {
        redraw = true;
        return;
    }
    switch (key) {
    case XK_c:
    case XK_C: {
        const int index = panel_at(mouse_position);
        for (std::size_t i = 0; i < panels.size(); ++i) {
            if (index >= 0 && static_cast<int>(i) != index)
                continue;
            for (auto& block : panels[i].folds)
                block.collapsed = !(event.xkey.state & ShiftMask);
            update_mesh(panels[i]);
        }
        relayout();
        connections_dirty = true;
        break;
    }
    case XK_i:
    case XK_I:
        select_intent_at(mouse_position);
        break;
    case XK_F1:
        show_shortcuts();
        break;
    case XK_Escape:
        drag_button = 0;
        XUngrabPointer(display, CurrentTime);
        break;
    }
    redraw = true;
}

bool Application::popup_button(const XEvent& event, bool in_popup)
{
    if (in_popup && (event.xbutton.button == Button4 || event.xbutton.button == Button5)) {
        popup_scroll += event.xbutton.button == Button4 ? -60 : 60;
        redraw = true;
        return true;
    }
    return in_popup && event.xbutton.button == Button1;
}

void Application::begin_drag(const XEvent& event, Vec2 pointer, const Viewport& view, bool pan_only)
{
    int header = -1;
    if (!pan_only && event.xbutton.button == Button1)
        for (std::size_t i = 0; i < header_hits.size(); ++i)
            if (Viewport{header_hits[i].origin.x, header_hits[i].origin.y, header_hits[i].size.x,
                         header_hits[i].size.y}
                    .contains(pointer))
                header = static_cast<int>(i);
    if (!pan_only && header < 0 && event.xbutton.button == Button1) {
        const auto fold = fold_at(pointer);
        if (fold.panel >= 0) {
            const auto bounds = fold_toggle_bounds(panels[fold.panel], fold.line);
            const auto world = camera.screen_to_world(pointer, view);
            if (Viewport{bounds.origin.x, bounds.origin.y, bounds.size.x, bounds.size.y}.contains(
                    world)) {
                toggle_fold(fold.panel, fold.line);
                return;
            }
        }
    }
    if (XGrabPointer(display, window, False,
                     ButtonPressMask | ButtonReleaseMask | PointerMotionMask, GrabModeAsync,
                     GrabModeAsync, None, header >= 0 ? normal_cursor : drag_cursor,
                     CurrentTime) == GrabSuccess) {
        drag_button = event.xbutton.button;
        last_pointer = pointer;
        zoom_animation.anchor = pointer;
        dragged_panel = header;
        drag_moved = false;
        if (header >= 0)
            window_manager.begin(panels[header].bounds.origin);
        if (header < 0)
            remember_view(true);
        update_cursor();
    }
}

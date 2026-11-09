#include "application.h"

void Application::handle_event(const XEvent& event)
{
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
            popup_dismiss_due = Clock::now() + std::chrono::milliseconds(300);
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
    const bool in_popup =
        !popup_text.empty() && Viewport{popup_bounds.origin.x, popup_bounds.origin.y,
                                        popup_bounds.size.x, popup_bounds.size.y}
                                   .contains(pointer);
    if (popup_button(event, pointer, in_popup))
        return;
    hide_popup();
    if (event.xbutton.button == Button4 || event.xbutton.button == Button5) {
        if (drag_button) {
            camera.pan({pointer.x - last_pointer.x, pointer.y - last_pointer.y});
            last_pointer = pointer;
        }
        zoom_animation.request(camera, event.xbutton.button == Button4 ? 1.15 : 1 / 1.15,
                               drag_button || view.contains(pointer) ? pointer : view.center());
    } else if (event.xbutton.button == Button1 && (event.xbutton.state & ControlMask)) {
        if (definition_at_pointer())
            list_request = query_at_pointer("textDocument/references");
        else
            navigation_request = query_at_pointer("textDocument/definition");
    } else if ((event.xbutton.button == Button1 || event.xbutton.button == Button2) &&
               !drag_button && view.contains(pointer)) {
        begin_drag(event, pointer, view);
    }
    redraw = true;
}

void Application::handle_motion(const XEvent& event)
{
    const Vec2 pointer{static_cast<double>(event.xmotion.x), static_cast<double>(event.xmotion.y)};
    mouse_position = pointer;
    redraw = true;
    update_cursor();
    if (drag_button) {
        hide_popup();
        const Vec2 delta{pointer.x - last_pointer.x, pointer.y - last_pointer.y};
        drag_moved = drag_moved || std::hypot(delta.x, delta.y) > 1;
        if (dragged_panel >= 0) {
            auto& position = panels[dragged_panel].bounds.origin;
            position.x = std::max(0.0, position.x + delta.x / camera.zoom);
            position.y = std::max(0.0, position.y + delta.y / camera.zoom);
            content_size.x =
                std::max(content_size.x, position.x + panels[dragged_panel].bounds.size.x);
            content_size.y =
                std::max(content_size.y, position.y + panels[dragged_panel].bounds.size.y);
            connections_dirty = true;
        } else
            camera.pan(delta);
        last_pointer = pointer;
        zoom_animation.anchor = pointer;
        redraw = true;
    } else if (popup_text.empty() || !Viewport{popup_bounds.origin.x, popup_bounds.origin.y,
                                               popup_bounds.size.x, popup_bounds.size.y}
                                          .contains(pointer)) {
        if (!popup_text.empty() && popup_dismiss_due == Clock::time_point::max())
            popup_dismiss_due = Clock::now() + std::chrono::milliseconds(300);
        hover_due = Clock::now() + std::chrono::milliseconds(400);
    } else {
        popup_dismiss_due = Clock::time_point::max();
        hover_due = Clock::time_point::max();
    }
}

void Application::handle_release(const XEvent& event)
{
    drag_button = 0;
    XUngrabPointer(display, CurrentTime);
    if (dragged_panel >= 0 && !drag_moved) {
        popup_origin = {static_cast<double>(event.xbutton.x), static_cast<double>(event.xbutton.y)};
        show_outline(dragged_panel);
    }
    dragged_panel = -1;
    update_cursor();
}

void Application::handle_key(const XEvent& event)
{
    const auto view = viewport();
    hide_popup();
    auto key_event = event.xkey;
    switch (XLookupKeysym(&key_event, 0)) {
    case XK_Up:
        camera.pan({0, 40});
        break;
    case XK_Down:
        camera.pan({0, -40});
        break;
    case XK_Left:
        if (!(event.xkey.state & Mod1Mask) || !go_back())
            camera.pan({40, 0});
        break;
    case XK_Right:
        camera.pan({-40, 0});
        break;
    case XK_Page_Up:
        camera.pan({0, view.height * 0.9});
        break;
    case XK_Page_Down:
        camera.pan({0, -view.height * 0.9});
        break;
    case XK_Home:
        camera.home(view);
        zoom_animation.cancel(camera);
        break;
    case XK_End:
        camera.home(view);
        camera.center.y += std::max(0.0, content_size.y - view.height / camera.zoom);
        zoom_animation.cancel(camera);
        break;
    case XK_0:
    case XK_KP_0:
        camera.reset(view);
        zoom_animation.cancel(camera);
        break;
    case XK_f:
    case XK_F:
        camera.fit(content_size, view);
        zoom_animation.cancel(camera);
        break;
    case XK_w:
    case XK_W:
        fit_width();
        break;
    case XK_g:
    case XK_G:
        show_connections = !show_connections;
        break;
    case XK_l:
    case XK_L:
        relayout();
        connections_dirty = true;
        camera.fit(content_size, view);
        zoom_animation.cancel(camera);
        break;
    case XK_s:
    case XK_S:
        popup_origin = mouse_position;
        show_outline(panel_at(mouse_position));
        break;
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
    case XK_k:
    case XK_K:
        hover_request = query_at_pointer("textDocument/hover");
        break;
    case XK_F1:
        show_popup(
            "Hover or K — inspect symbol\nCtrl+click or F12 — go to "
            "definition\nShift+F12 — references (click to jump)\nCtrl+Space — "
            "completion suggestions\nAlt+Left — go back\n\nWheel — smooth "
            "zoom\nBlank/middle drag — pan\nDrag file title — move file\nClick "
            "title or S — symbols\nHover block header, click ▸/▾ — fold block\nC / Shift+C — "
            "collapse / expand\nL — Graphviz layout   G — arrows\nW — fit width "
            "  F — fit all   0 — reset",
            mouse_position);
        break;
    case XK_F12:
        if (event.xkey.state & ShiftMask)
            list_request = query_at_pointer("textDocument/references");
        else
            navigation_request = query_at_pointer("textDocument/definition");
        break;
    case XK_space:
        if (event.xkey.state & ControlMask)
            list_request = query_at_pointer("textDocument/completion");
        break;
    case XK_equal:
    case XK_plus:
    case XK_KP_Add:
        zoom_animation.request(camera, 1.15, view.center());
        break;
    case XK_minus:
    case XK_KP_Subtract:
        zoom_animation.request(camera, 1 / 1.15, view.center());
        break;
    case XK_Escape:
        drag_button = 0;
        XUngrabPointer(display, CurrentTime);
        break;
    }
    redraw = true;
}

bool Application::popup_button(const XEvent& event, Vec2 pointer, bool in_popup)
{
    if (in_popup && (event.xbutton.button == Button4 || event.xbutton.button == Button5)) {
        if (!jump_rows.empty()) {
            list_offset += event.xbutton.button == Button4 ? -3 : 3;
            refresh_list();
        } else
            popup_scroll += event.xbutton.button == Button4 ? -60 : 60;
        redraw = true;
        return true;
    }
    if (event.xbutton.button == Button1 && !popup_locations.empty() &&
        Viewport{popup_bounds.origin.x, popup_bounds.origin.y, popup_bounds.size.x,
                 popup_bounds.size.y}
            .contains(pointer)) {
        int index = 0, trailing = 0;
        pango_layout_xy_to_index(
            popup_layout, static_cast<int>((pointer.x - popup_bounds.origin.x - 12) * PANGO_SCALE),
            static_cast<int>((pointer.y - popup_bounds.origin.y - 12 + popup_scroll) * PANGO_SCALE),
            &index, &trailing);
        const int row =
            static_cast<int>(std::count(
                popup_text.begin(),
                popup_text.begin() + std::clamp(index, 0, static_cast<int>(popup_text.size())),
                '\n')) -
            1;
        if (row >= 0 && row < static_cast<int>(popup_locations.size())) {
            if (row < static_cast<int>(popup_symbol_rows.size()) && popup_symbol_rows[row] &&
                !popup_symbol_rows[row]->children.empty() &&
                pointer.x - popup_bounds.origin.x < 12 + popup_row_depths[row] * 17 + 25) {
                popup_symbol_rows[row]->expanded = !popup_symbol_rows[row]->expanded;
                show_outline(outline_panel);
                return true;
            }
            const auto location = popup_locations[row];
            jump_to(location);
        }
        return true;
    }

    return in_popup && event.xbutton.button == Button1;
}

void Application::begin_drag(const XEvent& event, Vec2 pointer, const Viewport& view)
{
    int header = -1;
    if (event.xbutton.button == Button1)
        for (std::size_t i = 0; i < header_hits.size(); ++i)
            if (Viewport{header_hits[i].origin.x, header_hits[i].origin.y, header_hits[i].size.x,
                         header_hits[i].size.y}
                    .contains(pointer))
                header = static_cast<int>(i);
    if (header < 0 && event.xbutton.button == Button1) {
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
    // Keep receiving wheel presses while either pan button is held.
    if (XGrabPointer(display, window, False,
                     ButtonPressMask | ButtonReleaseMask | PointerMotionMask, GrabModeAsync,
                     GrabModeAsync, None, header >= 0 ? normal_cursor : drag_cursor,
                     CurrentTime) == GrabSuccess) {
        drag_button = event.xbutton.button;
        last_pointer = pointer;
        zoom_animation.anchor = pointer;
        dragged_panel = header;
        drag_moved = false;
        update_cursor();
    }
}

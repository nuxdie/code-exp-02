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
        if (!walkthrough_event(event, {static_cast<double>(event.xmotion.x),
                                       static_cast<double>(event.xmotion.y)}))
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
            popup_dismiss_due =
                search.active || shortcuts_visible || options_page != OptionsPage::Closed
                    ? Clock::time_point::max()
                    : Clock::now() + std::chrono::milliseconds(300);
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
    if (walkthrough_event(event, pointer))
        return;
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
    if (event.xbutton.button == Button1 && !drag_button)
        activate_chat(pointer);
    const bool in_popup =
        !popup_text.empty() && Viewport{popup_bounds.origin.x, popup_bounds.origin.y,
                                        popup_bounds.size.x, popup_bounds.size.y}
                                   .contains(pointer);
    if (begin_text_selection(event, pointer) || popup_button(event, pointer, in_popup) ||
        intent_button(event, pointer) || chat_button(event, pointer))
        return;
    schedule_save();
    hide_popup();
    if (event.xbutton.button == Button1 && !(event.xbutton.state & Mod1Mask) &&
        open_document_link(pointer))
        return;
    if (event.xbutton.button == Button4 || event.xbutton.button == Button5) {
        zoom_wheel(event.xbutton.button, pointer);
    } else if (event.xbutton.button == Button1 && (event.xbutton.state & Mod1Mask)) {
        inspect_intent(pointer);
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

bool Application::close_window_button(const XEvent& event, Vec2 pointer)
{
    if (event.xbutton.button != Button2 || drag_button)
        return false;
    auto contains = [](const CanvasRect& bounds, Vec2 point) {
        return Viewport{bounds.origin.x, bounds.origin.y, bounds.size.x, bounds.size.y}.contains(
            point);
    };
    // Match the drawing order: search, popups, intent, then conversations.
    if (search.active) {
        if (!contains(search.bounds, pointer))
            return false;
        close_search();
    } else if (!popup_text.empty() && contains(popup_bounds, pointer)) {
        if (text_selection.surface == TextSurface::Popup)
            text_selection = {};
        hide_popup();
    } else if (intent && intent->placed && intent->visible &&
               contains(intent->bounds, camera.screen_to_world(pointer, viewport()))) {
        intent->visible = intent->editing = false;
        if (text_selection.surface == TextSurface::Intent ||
            text_selection.surface == TextSurface::Rationale)
            text_selection = {};
        schedule_save();
    } else if (activate_chat(pointer))
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
        if ((!chat || !chat->scrolling) && (!intent || !intent->scrolling) &&
            !text_selection.dragging)
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
    if ((search.active || shortcuts_visible || options_page != OptionsPage::Closed) &&
        !drag_button) {
        hover_due = popup_dismiss_due = Clock::time_point::max();
        return;
    }
    if (text_selection.dragging) {
        move_text_selection(pointer);
        return;
    }
    if (drag_button) {
        hide_popup();
        const Vec2 delta{pointer.x - last_pointer.x, pointer.y - last_pointer.y};
        drag_moved = drag_moved || std::hypot(delta.x, delta.y) > 1;
        if (chat && chat->scrolling) {
            drag_chat_scroll(camera.screen_to_world(pointer, viewport()));
        } else if (intent && intent->scrolling) {
            intent->drag_scroll(camera.screen_to_world(pointer, viewport()));
        } else if (intent && intent->dragging) {
            intent->anchor_path.clear();
            intent->bounds.origin.x += delta.x / camera.zoom;
            intent->bounds.origin.y += delta.y / camera.zoom;
        } else if (chat && chat->dragging) {
            chat->bounds.origin.x += delta.x / camera.zoom;
            chat->bounds.origin.y += delta.y / camera.zoom;
        } else if (dragged_panel >= 0) {
            auto& position = panels[dragged_panel].bounds.origin;
            position.x = std::max(0.0, position.x + delta.x / camera.zoom);
            position.y = std::max(0.0, position.y + delta.y / camera.zoom);
            content_size.x =
                std::max(content_size.x, position.x + panels[dragged_panel].bounds.size.x);
            content_size.y =
                std::max(content_size.y, position.y + panels[dragged_panel].bounds.size.y);
            connections_dirty = true;
        } else {
            remember_view(true);
            camera.pan(delta);
        }
        last_pointer = pointer;
        zoom_animation.anchor = pointer;
        redraw = true;
    } else if (popup_text.empty() || !Viewport{popup_bounds.origin.x, popup_bounds.origin.y,
                                               popup_bounds.size.x, popup_bounds.size.y}
                                          .contains(pointer)) {
        if (!popup_text.empty() && popup_dismiss_due == Clock::time_point::max())
            popup_dismiss_due = Clock::now() + std::chrono::milliseconds(300);
        const auto world = camera.screen_to_world(pointer, viewport());
        const bool in_intent = intent && intent->visible &&
                               Viewport{intent->bounds.origin.x, intent->bounds.origin.y,
                                        intent->bounds.size.x, intent->bounds.size.y}
                                   .contains(world);
        const bool in_chat = chat_at(pointer) != nullptr;
        hover_due = in_chat || in_intent ? Clock::time_point::max()
                                         : Clock::now() + std::chrono::milliseconds(400);
    } else {
        popup_dismiss_due = Clock::time_point::max();
        hover_due = Clock::time_point::max();
    }
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
    if (intent && intent->scrolling)
        intent->drag_scroll(camera.screen_to_world(
            {static_cast<double>(event.xbutton.x), static_cast<double>(event.xbutton.y)},
            viewport()));
    drag_button = 0;
    XUngrabPointer(display, CurrentTime);
    if (dragged_panel >= 0 && !drag_moved) {
        popup_origin = {static_cast<double>(event.xbutton.x), static_cast<double>(event.xbutton.y)};
        show_outline(dragged_panel);
    }
    dragged_panel = -1;
    dragged_walkthrough_step = -1;
    if (chat) {
        chat->dragging = false;
        chat->scrolling = false;
    }
    if (intent) {
        intent->dragging = false;
        intent->scrolling = false;
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
        (!text_selection.dragging && (!chat || !chat->scrolling) &&
         (!intent || !intent->scrolling)))
        return false;
    text_selection.moved = true;
    handle_release(event);
    return true;
}

void Application::handle_key(const XEvent& event)
{
    if (cancel_pointer_drag(event) || canvas_shortcut_key(event) || agent_window_key(event) ||
        selection_key(event) || intent_key(event) || chat_key(event)) {
        redraw = true;
        return;
    }
    auto escape_event = event.xkey;
    if (XLookupKeysym(&escape_event, 0) == XK_Escape) {
        if (!popup_text.empty())
            hide_popup();
        else if (!drag_button)
            show_options();
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
    case XK_g:
    case XK_G:
        show_connections = !show_connections;
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
    case XK_i:
    case XK_I:
        inspect_intent(mouse_position);
        break;
    case XK_d:
    case XK_D:
        intent_action("all:");
        break;
    case XK_k:
    case XK_K:
        hover_request = query_at_pointer("textDocument/hover");
        break;
    case XK_F1:
        show_shortcuts();
        break;
    case XK_F12:
        if (!(event.xkey.state & ShiftMask) && open_document_link(mouse_position))
            break;
        if (event.xkey.state & ShiftMask)
            list_request = query_at_pointer("textDocument/references");
        else
            navigation_request = query_at_pointer("textDocument/definition");
        break;
    case XK_space:
        if (event.xkey.state & ControlMask)
            list_request = query_at_pointer("textDocument/completion");
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
    if (in_popup && options_page != OptionsPage::Closed && event.xbutton.button == Button1) {
        int index = 0, trailing = 0;
        pango_layout_xy_to_index(
            popup_layout, static_cast<int>((pointer.x - popup_bounds.origin.x - 12) * PANGO_SCALE),
            static_cast<int>((pointer.y - popup_bounds.origin.y - 12 + popup_scroll) * PANGO_SCALE),
            &index, &trailing);
        const int row = static_cast<int>(std::count(
            popup_text.begin(),
            popup_text.begin() + std::clamp(index, 0, static_cast<int>(popup_text.size())), '\n'));
        options_action(row);
        return true;
    }
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
            (list_title.empty() ? 0 : 1);
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
        if (header < 0)
            remember_view(true);
        update_cursor();
    }
}

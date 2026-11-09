#include "application.h"

bool Application::over_text(Vec2 screen)
{
    if (!popup_text.empty() && Viewport{popup_bounds.origin.x, popup_bounds.origin.y,
                                        popup_bounds.size.x, popup_bounds.size.y}
                                   .contains(screen)) {
        int index = 0, trailing = 0;
        return pango_layout_xy_to_index(
            popup_layout, static_cast<int>((screen.x - popup_bounds.origin.x - 12) * PANGO_SCALE),
            static_cast<int>((screen.y - popup_bounds.origin.y - 12 + popup_scroll) * PANGO_SCALE),
            &index, &trailing);
    }
    for (const auto& header : header_hits)
        if (Viewport{header.origin.x, header.origin.y, header.size.x, header.size.y}.contains(
                screen))
            return false;
    const auto world = camera.screen_to_world(screen, viewport());
    if (chat && Viewport{chat->bounds.origin.x, chat->bounds.origin.y, chat->bounds.size.x,
                         chat->bounds.size.y}
                    .contains(world))
        return chat_over_text(screen);
    if (intent && intent->placed &&
        Viewport{intent->bounds.origin.x, intent->bounds.origin.y, intent->bounds.size.x,
                 intent->bounds.size.y}
            .contains(world)) {
        if (intent->editing && Viewport{intent->editor.origin.x, intent->editor.origin.y,
                                        intent->editor.size.x, intent->editor.size.y}
                                   .contains(world))
            return true;
        int index = 0, trailing = 0;
        return intent->document.layout &&
               Viewport{intent->body.origin.x, intent->body.origin.y, intent->body.size.x,
                        intent->body.size.y}
                   .contains(world) &&
               pango_layout_xy_to_index(
                   intent->document.layout.get(),
                   static_cast<int>((world.x - intent->body.origin.x) * PANGO_SCALE),
                   static_cast<int>((world.y - intent->body.origin.y + intent->scroll) *
                                    PANGO_SCALE),
                   &index, &trailing);
    }
    for (const auto& panel : panels) {
        const double x = world.x - panel.bounds.origin.x - 24;
        const double y = world.y - panel.bounds.origin.y - header_height;
        if (x < 0 || y < 0)
            continue;
        const int row = static_cast<int>(y / line_height);
        if (row >= static_cast<int>(panel.folded.source_lines.size()))
            continue;
        int index = 0, trailing = 0;
        if (pango_layout_xy_to_index(panel.lines[panel.folded.source_lines[row]],
                                     static_cast<int>(x * PANGO_SCALE),
                                     font_height * PANGO_SCALE / 2, &index, &trailing))
            return true;
    }
    return false;
}

void Application::update_cursor()
{
    Cursor cursor = normal_cursor;
    if (drag_button) {
        if (dragged_panel < 0 && (!chat || !chat->dragging) && (!intent || !intent->dragging))
            cursor = drag_cursor;
    } else if (viewport().contains(mouse_position) && over_text(mouse_position)) {
        cursor = text_cursor;
    }
    if (cursor != active_cursor) {
        XDefineCursor(display, window, cursor);
        active_cursor = cursor;
    }
}

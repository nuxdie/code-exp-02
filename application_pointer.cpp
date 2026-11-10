#include "application.h"

bool Application::over_text(Vec2 screen)
{
    const auto target = text_at(screen);
    bool inside = false;
    text_byte_at(target, screen, &inside);
    return inside;
}

void Application::update_cursor()
{
    Cursor cursor = normal_cursor;
    if (text_selection.dragging)
        cursor = text_cursor;
    else if (drag_button) {
        if (dragged_panel < 0 && (!chat || (!chat->dragging && !chat->scrolling)) &&
            (!intent || !intent->dragging))
            cursor = drag_cursor;
    } else if (viewport().contains(mouse_position) && over_text(mouse_position)) {
        cursor = text_link_at(mouse_position).empty() ? text_cursor : link_cursor;
    } else if (intent && !search.active && popup_text.empty() && !chat_at(mouse_position)) {
        for (const auto& badge : intent->badge_hits)
            if (Viewport{badge.bounds.origin.x, badge.bounds.origin.y, badge.bounds.size.x,
                         badge.bounds.size.y}
                    .contains(mouse_position))
                cursor = link_cursor;
    }
    if (cursor != active_cursor) {
        XDefineCursor(display, window, cursor);
        active_cursor = cursor;
    }
}

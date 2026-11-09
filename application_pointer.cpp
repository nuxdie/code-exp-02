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
        cursor = text_cursor;
    }
    if (cursor != active_cursor) {
        XDefineCursor(display, window, cursor);
        active_cursor = cursor;
    }
}

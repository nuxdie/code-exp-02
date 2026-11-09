#include "application.h"
#include <X11/Xatom.h>

TextInput& Application::selected_input()
{
    if (text_selection.surface == TextSurface::Composer)
        return chat->input;
    if (text_selection.surface == TextSurface::Rationale)
        return intent->input;
    return text_selection.range;
}
bool Application::begin_text_selection(const XEvent& event, Vec2 pointer)
{
    if (event.xbutton.button != Button1 || drag_button ||
        (event.xbutton.state & (ControlMask | Mod1Mask)))
        return false;
    const auto target = text_at(pointer);
    bool inside = false;
    const auto byte = text_byte_at(target, pointer, &inside);
    if (!target.text || !inside) {
        text_selection = {};
        return false;
    }
    if (XGrabPointer(display, window, False,
                     ButtonPressMask | ButtonReleaseMask | PointerMotionMask, GrabModeAsync,
                     GrabModeAsync, None, text_cursor, CurrentTime) != GrabSuccess)
        return false;
    const bool extend =
        (event.xbutton.state & ShiftMask) && text_selection.surface == target.surface &&
        text_selection.key == target.key && text_selection.range.text == *target.text;
    if (!extend)
        text_selection.range.set(*target.text);
    text_selection.surface = target.surface;
    text_selection.key = target.key;
    text_selection.math = target.math ? *target.math : std::vector<PopupMath>{};
    text_selection.press = pointer;
    text_selection.dragging = true;
    text_selection.moved = false;
    text_selection.extend = extend;
    selected_input().place(byte, extend);
    chat->focused = target.surface == TextSurface::Composer;
    if (target.surface != TextSurface::Rationale && intent)
        intent->editing = false;
    drag_button = Button1;
    dragged_panel = -1;
    last_pointer = pointer;
    hover_due = popup_dismiss_due = Clock::time_point::max();
    update_cursor();
    return true;
}
void Application::move_text_selection(Vec2 pointer)
{
    const auto target = active_text_target();
    if (!target.text || *target.text != text_selection.range.text) {
        text_selection.moved = true;
        return;
    }
    if (!text_selection.moved &&
        std::hypot(pointer.x - text_selection.press.x, pointer.y - text_selection.press.y) < 3)
        return;
    text_selection.moved = true;
    selected_input().place(text_byte_at(target, pointer), true);
    hover_due = popup_dismiss_due = Clock::time_point::max();
}
void Application::end_text_selection(const XEvent& event)
{
    const Vec2 pointer{static_cast<double>(event.xbutton.x), static_cast<double>(event.xbutton.y)};
    move_text_selection(pointer);
    const bool click = !text_selection.moved && !text_selection.extend;
    text_selection.dragging = false;
    drag_button = 0;
    XUngrabPointer(display, CurrentTime);
    primary_text = selected_text();
    if (!primary_text.empty())
        XSetSelectionOwner(display, XA_PRIMARY, window, event.xbutton.time);
    if (click)
        selection_click(event);
    redraw = true;
    schedule_save();
    update_cursor();
}
void Application::selection_click(const XEvent& event)
{
    const Vec2 pointer{static_cast<double>(event.xbutton.x), static_cast<double>(event.xbutton.y)};
    if (text_selection.surface == TextSurface::Source) {
        open_document_link(pointer);
    } else if (text_selection.surface == TextSurface::Intent) {
        auto click = event;
        click.type = ButtonPress;
        intent_button(click, pointer);
    } else if (text_selection.surface == TextSurface::Popup) {
        popup_button(event, pointer, true);
    } else if (text_selection.surface == TextSurface::Chat) {
        for (std::size_t i = 0; i < chat->conversation.entries.size(); ++i)
            if (chat->conversation.entries[i].id == text_selection.key) {
                const auto target = active_text_target();
                const auto world = camera.screen_to_world(pointer, viewport());
                const auto origin = camera.screen_to_world(target.origin, viewport());
                chat_action({ChatHit::Kind::Message, {origin, {}}, i}, world);
                break;
            }
    }
}
std::string Application::selected_text()
{
    if (text_selection.surface == TextSurface::Composer ||
        text_selection.surface == TextSurface::Rationale)
        return selected_input().selected();
    const auto target = active_text_target();
    if (!target.text || *target.text != text_selection.range.text)
        return {};
    const auto [begin, end] = selected_input().selection();
    auto result = text_selection.range.text.substr(begin, end - begin);
    for (auto math = text_selection.math.rbegin(); math != text_selection.math.rend(); ++math) {
        if (math->begin < begin || math->begin + 3 > end)
            continue;
        const auto delimiter = math->display ? "$$" : "$";
        result.replace(math->begin - begin, 3, delimiter + math->expression + delimiter);
    }
    return result;
}
bool Application::selection_key(const XEvent& event)
{
    auto key_event = event.xkey;
    const auto key = XLookupKeysym(&key_event, 0);
    if (text_selection.surface == TextSurface::Inactive)
        return false;
    if ((key == XK_c || key == XK_C) && (event.xkey.state & ControlMask)) {
        const auto text = selected_text();
        if (text.empty())
            return false;
        clipboard_text = text;
        XSetSelectionOwner(display, XInternAtom(display, "CLIPBOARD", False), window,
                           event.xkey.time);
        return true;
    }
    if ((key == XK_a || key == XK_A) && (event.xkey.state & ControlMask)) {
        selected_input().select_all();
        primary_text = selected_text();
        if (!primary_text.empty())
            XSetSelectionOwner(display, XA_PRIMARY, window, event.xkey.time);
        redraw = true;
        return true;
    }
    if (key == XK_Escape && !text_selection.dragging) {
        selected_input().place(selected_input().cursor);
        text_selection = {};
        redraw = true;
        return true;
    }
    return false;
}

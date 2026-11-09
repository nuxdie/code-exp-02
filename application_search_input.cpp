#include "application.h"
#include <X11/Xutil.h>

namespace {
bool contains(const CanvasRect& bounds, Vec2 point)
{
    return Viewport{bounds.origin.x, bounds.origin.y, bounds.size.x, bounds.size.y}.contains(point);
}
std::string input_text(XIC context, XKeyEvent event)
{
    KeySym symbol;
    Status status = 0;
    std::string text(256, '\0');
    int length = context ? Xutf8LookupString(context, &event, text.data(),
                                             static_cast<int>(text.size()), &symbol, &status)
                         : XLookupString(&event, text.data(), static_cast<int>(text.size()),
                                         &symbol, nullptr);
    if (status == XBufferOverflow) {
        text.resize(static_cast<std::size_t>(length));
        length = Xutf8LookupString(context, &event, text.data(), length, &symbol, &status);
    }
    if (length <= 0 || !g_utf8_validate(text.c_str(), length, nullptr))
        return {};
    text.resize(static_cast<std::size_t>(length));
    return text;
}
} // namespace
bool Application::search_edit_key(const XEvent& event, KeySym key)
{
    const auto state = event.xkey.state;
    const bool shift = state & ShiftMask;
    if (state & ControlMask) {
        switch (key) {
        case XK_a:
            search.query.select_all();
            return true;
        case XK_c:
        case XK_x:
            clipboard_text = search.query.selected();
            XSetSelectionOwner(display, XInternAtom(display, "CLIPBOARD", False), window,
                               event.xkey.time);
            if (key == XK_x)
                search.query.insert("");
            return true;
        case XK_v:
            XConvertSelection(display, XInternAtom(display, "CLIPBOARD", False),
                              XInternAtom(display, "UTF8_STRING", False),
                              XInternAtom(display, "SOURCE_CANVAS_SEARCH_PASTE", False), window,
                              event.xkey.time);
            return true;
        default:
            return false;
        }
    }
    switch (key) {
    case XK_BackSpace:
        search.query.erase(true);
        return true;
    case XK_Delete:
        search.query.erase(false);
        return true;
    case XK_Left:
    case XK_Right:
        search.query.move(key == XK_Left ? -1 : 1, shift);
        return true;
    case XK_Home:
    case XK_End:
        search.query.line_end(key == XK_End, shift);
        return true;
    default:
        return false;
    }
}
bool Application::search_key(const XEvent& event)
{
    auto key_event = event.xkey;
    const auto key = XLookupKeysym(&key_event, 0);
    const auto before = search.query.text;
    switch (key) {
    case XK_Escape:
        close_search();
        return true;
    case XK_Tab:
        move_search(event.xkey.state & ShiftMask ? -1 : 1);
        return true;
    case XK_Return:
    case XK_KP_Enter:
        open_search_match(search.selected);
        return true;
    case XK_Up:
        move_search(-1);
        return true;
    case XK_Down:
        move_search(1);
        return true;
    case XK_Page_Up:
        move_search(-8);
        return true;
    case XK_Page_Down:
        move_search(8);
        return true;
    default:
        break;
    }
    if (!search_edit_key(event, key) && !(event.xkey.state & (ControlMask | Mod1Mask))) {
        const auto text = input_text(input_context, event.xkey);
        if (search.query.text.size() + text.size() <= 1024)
            search.query.insert(text);
    }
    if (before != search.query.text)
        update_search();
    return true;
}
bool Application::search_button(const XEvent& event, Vec2 pointer)
{
    if (!search.active)
        return false;
    if (event.xbutton.button == Button4 || event.xbutton.button == Button5) {
        move_search(event.xbutton.button == Button4 ? -3 : 3);
        return true;
    }
    if (event.xbutton.button != Button1)
        return true;
    if (!contains(search.bounds, pointer)) {
        close_search();
        return true;
    }
    if (contains(search.editor, pointer)) {
        if (!begin_text_selection(event, pointer))
            search.query.place(text_byte_at(text_at(pointer), pointer),
                               event.xbutton.state & ShiftMask);
        return true;
    }
    for (const auto& [bounds, index] : search.hits)
        if (contains(bounds, pointer)) {
            open_search_match(index);
            break;
        }
    return true;
}
void Application::paste_search(const XEvent& event)
{
    Atom type;
    int format;
    unsigned long count = 0, remaining = 0;
    unsigned char* bytes = nullptr;
    if (XGetWindowProperty(display, window, event.xselection.property, 0, 262144, True,
                           AnyPropertyType, &type, &format, &count, &remaining,
                           &bytes) != Success ||
        !bytes)
        return;
    if (search.active && format == 8 && count <= 1024 &&
        g_utf8_validate(reinterpret_cast<const char*>(bytes), static_cast<gssize>(count),
                        nullptr)) {
        std::string text(reinterpret_cast<const char*>(bytes), count);
        std::replace(text.begin(), text.end(), '\n', ' ');
        std::replace(text.begin(), text.end(), '\r', ' ');
        search.query.insert(text);
        update_search();
    }
    XFree(bytes);
}

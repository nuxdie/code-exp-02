#include "application.h"
#include <X11/Xutil.h>

namespace {
bool inside(const CanvasRect& bounds, Vec2 point)
{
    return Viewport{bounds.origin.x, bounds.origin.y, bounds.size.x, bounds.size.y}.contains(point);
}
std::string link_at(const DocumentView& document, Vec2 point)
{
    if (!document.layout)
        return {};
    int index = 0, trailing = 0;
    if (!pango_layout_xy_to_index(document.layout.get(), static_cast<int>(point.x * PANGO_SCALE),
                                  static_cast<int>(point.y * PANGO_SCALE), &index, &trailing))
        return {};
    for (const auto& span : document.document.styles)
        if (span.style == PopupStyle::Link && index >= static_cast<int>(span.begin) &&
            index < static_cast<int>(span.begin + span.length))
            return span.destination;
    return {};
}
} // namespace
bool Application::intent_button(const XEvent& event, Vec2 pointer)
{
    if (!intent || !intent->placed || drag_button || event.xbutton.button == Button2)
        return false;
    const auto world = camera.screen_to_world(pointer, viewport());
    if (!inside(intent->bounds, world))
        return false;
    if (event.xbutton.button == Button4 || event.xbutton.button == Button5)
        intent->scroll += (event.xbutton.button == Button4 ? -70 : 70) / camera.zoom;
    if (event.xbutton.button == Button1) {
        const Vec2 header{world.x - intent->bounds.origin.x - 24,
                          world.y - intent->bounds.origin.y - 16};
        auto link = link_at(intent->header, header);
        if (world.y > intent->bounds.origin.y + intent->bounds.size.y - 86)
            link = link_at(intent->controls, {header.x, world.y - intent->bounds.origin.y -
                                                            intent->bounds.size.y + 86});
        if (inside(intent->body, world) && !intent->editing) {
            const Vec2 point{world.x - intent->body.origin.x,
                             world.y - intent->body.origin.y + intent->scroll};
            link = link_at(intent->document, point);
            if (!link.empty() && link.compare(0, 7, "intent:") != 0 &&
                link.compare(0, 7, "commit:") != 0 && link.compare(0, 5, "chat:") != 0)
                open_chat_link(intent->document, point);
        }
        if (!link.empty())
            intent_action(link);
        else if (header.y < 24 &&
                 XGrabPointer(display, window, False,
                              ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                              GrabModeAsync, GrabModeAsync, None, normal_cursor,
                              CurrentTime) == GrabSuccess) {
            drag_button = Button1;
            intent->dragging = true;
            last_pointer = pointer;
        }
        chat->focused = false;
    }
    redraw = true;
    schedule_save();
    return true;
}
void Application::intent_action(const std::string& action)
{
    if (action == "all:") {
        intent->overview = true;
        intent->evidence.clear();
        intent->scroll = 0;
    } else if (action == "conversation:")
        focus_chat();
    else if (action == "refresh:")
        intent_request("decisions/list");
    else if (action.compare(0, 7, "intent:") == 0) {
        intent->selected = action.substr(7);
        intent->overview = false;
        intent->evidence.clear();
        intent->token.clear();
        intent->scroll = 0;
    } else if (action.compare(0, 7, "commit:") == 0)
        intent_request("evidence/commit", {{"commit", action.substr(7)}});
    else if (action.compare(0, 5, "chat:") == 0)
        open_intent_conversation(action.substr(5));
    else if (action == "why:") {
        intent->editing = true;
        intent->input.set("");
    } else if (action == "preview:" || action == "capture:") {
        lsp::Json params{{"decision_id", intent->selected}};
        if (action == "capture:")
            params["paths"] = lsp::Json::array();
        intent->overview = false;
        intent_request("evidence/preview", std::move(params));
    } else if (action == "checkpoint:")
        intent_request("evidence/checkpoint", {{"token", intent->token}});
    else if (action == "cancel:") {
        intent->token.clear();
        intent->evidence.clear();
    }
}
void Application::open_intent_conversation(const std::string& id)
{
    for (const auto& record : intent->records) {
        if (record.value("id", "") != id)
            continue;
        const auto origin = record.value("origin", lsp::Json::object());
        const auto thread = origin.value("thread_id", "");
        if (thread != chat->conversation.thread_id && !thread.empty())
            chat->conversation.select_thread(thread);
        chat->restore_anchor = origin.value("message_id", "");
        chat->restore_offset = 0;
        focus_chat();
        chat->follow = false;
        return;
    }
}
bool Application::intent_key(const XEvent& event)
{
    if (!intent || !intent->editing)
        return false;
    auto key_event = event.xkey;
    const auto key = XLookupKeysym(&key_event, 0);
    if (key == XK_Escape)
        intent->editing = false;
    else if (key == XK_Return) {
        intent_request("decisions/rationale", {{"decision_id", intent->selected},
                                               {"rationale",
                                                {{"summary", intent->input.text},
                                                 {"author_role", "human"},
                                                 {"basis", "explicit_human_note"}}}});
        intent->editing = false;
    } else if (key == XK_BackSpace)
        intent->input.erase(true);
    else if (key == XK_Delete)
        intent->input.erase(false);
    else if (key == XK_Left || key == XK_Right)
        intent->input.move(key == XK_Left ? -1 : 1, event.xkey.state & ShiftMask);
    else {
        KeySym symbol;
        Status status = 0;
        std::string text(256, '\0');
        const auto length = input_context
                                ? Xutf8LookupString(input_context, &key_event, text.data(),
                                                    static_cast<int>(text.size()), &symbol, &status)
                                : XLookupString(&key_event, text.data(),
                                                static_cast<int>(text.size()), &symbol, nullptr);
        if (length > 0 && status != XBufferOverflow &&
            !(event.xkey.state & (ControlMask | Mod1Mask))) {
            text.resize(static_cast<std::size_t>(length));
            if (g_utf8_validate(text.c_str(), static_cast<gssize>(text.size()), nullptr))
                intent->input.insert(text);
        }
    }
    redraw = true;
    return true;
}

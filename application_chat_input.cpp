#include "application.h"
#include <X11/Xatom.h>
#include <X11/Xutil.h>

namespace {
bool contains(const CanvasRect& bounds, Vec2 point)
{
    return Viewport{bounds.origin.x, bounds.origin.y, bounds.size.x, bounds.size.y}.contains(point);
}
} // namespace

bool Application::chat_button(const XEvent& event, Vec2 pointer)
{
    if (!chat || !chat->visible || drag_button || event.xbutton.button == Button2 ||
        event.xbutton.button == Button4 || event.xbutton.button == Button5)
        return false;
    const auto world = camera.screen_to_world(pointer, viewport());
    if (chat_at(pointer) != chat.get()) {
        if (event.xbutton.button == Button1)
            chat->focused = false;
        return false;
    }
    if (event.xbutton.button == Button1) {
        ChatHit action{ChatHit::Kind::Title, {}};
        if (!contains(chat->header_bounds, pointer))
            for (auto hit = chat->hits.rbegin(); hit != chat->hits.rend(); ++hit)
                if (contains(hit->bounds, world)) {
                    action = *hit;
                    break;
                }
        const bool text = chat_over_text(pointer);
        if (action.kind == ChatHit::Kind::Message && (event.xbutton.state & Mod1Mask))
            attribute_chat_entry(action.index);
        else if ((action.kind == ChatHit::Kind::Message || action.kind == ChatHit::Kind::Text) &&
                 !text)
            chat_action({ChatHit::Kind::Title, {}}, world);
        else if (action.kind != ChatHit::Kind::Title || !text)
            chat_action(action, world);
    }
    hide_popup();
    schedule_save();
    redraw = true;
    return true;
}
void Application::chat_action(const ChatHit& hit, Vec2 world)
{
    switch (hit.kind) {
    case ChatHit::Kind::Title:
        chat->focused = false;
        if (XGrabPointer(display, window, False,
                         ButtonPressMask | ButtonReleaseMask | PointerMotionMask, GrabModeAsync,
                         GrabModeAsync, None, normal_cursor, CurrentTime) == GrabSuccess) {
            drag_button = Button1;
            chat->dragging = true;
            last_pointer = mouse_position;
        }
        break;
    case ChatHit::Kind::Text:
        break;
    case ChatHit::Kind::Scrollbar:
        begin_chat_scroll(world);
        break;
    case ChatHit::Kind::Input: {
        chat->focused = true;
        int index = 0, trailing = 0;
        if (chat->composer.layout && !chat->input.text.empty())
            pango_layout_xy_to_index(
                chat->composer.layout.get(),
                static_cast<int>((world.x - chat->editor.origin.x - 12) * PANGO_SCALE),
                static_cast<int>((world.y - chat->editor.origin.y - 10 + chat->editor_scroll) *
                                 PANGO_SCALE),
                &index, &trailing);
        const auto byte =
            std::min(static_cast<std::size_t>(std::max(0, index)), chat->input.text.size());
        const char* text = chat->input.text.c_str();
        chat->input.place(
            static_cast<std::size_t>(g_utf8_offset_to_pointer(text + byte, trailing) - text));
        break;
    }
    case ChatHit::Kind::Send:
        refresh_agent_context();
        if (!chat->conversation.questions.empty() &&
            chat->conversation.questions.front().method == "item/tool/requestUserInput")
            respond_chat_question(chat->input.text);
        else if (chat->conversation.submit(chat->input.text) && chat->composing) {
            chat->composing = false;
            chat->bounds.origin = {chat->editor.origin.x - 20, chat->editor.origin.y - 788};
            chat->bounds.size = {760, 900};
            chat->follow = true;
            content_size.x = std::max(content_size.x, chat->bounds.origin.x + chat->bounds.size.x);
            content_size.y = std::max(content_size.y, chat->bounds.origin.y + chat->bounds.size.y);
            focus_chat();
        }
        break;
    case ChatHit::Kind::Stop:
        chat->conversation.interrupt();
        break;
    case ChatHit::Kind::Threads:
        chat->choosing_thread = !chat->choosing_thread;
        chat->conversation.list_threads();
        break;
    case ChatHit::Kind::Thread:
        if (hit.index < chat->conversation.threads.size()) {
            chat->drafts[chat->conversation.thread_id] = chat->input.text;
            chat->conversation.select_thread(chat->conversation.threads[hit.index].value("id", ""));
            chat->input.set(chat->drafts[chat->conversation.thread_id]);
            chat->choosing_thread = false;
            chat->messages.clear();
            chat->follow = true;
        }
        break;
    case ChatHit::Kind::Message: {
        if (!contains(chat->body, world))
            break;
        auto& entry = chat->conversation.entries[hit.index];
        if (entry.activity)
            entry.expanded = !entry.expanded;
        else {
            const auto found = chat->messages.find(entry.id);
            if (found != chat->messages.end())
                open_chat_link(found->second,
                               {world.x - hit.bounds.origin.x, world.y - hit.bounds.origin.y});
        }
        break;
    }
    case ChatHit::Kind::Accept:
        chat->conversation.answer(0, true, chat->input.text);
        break;
    case ChatHit::Kind::Decline:
        chat->conversation.answer(0, false);
        break;
    case ChatHit::Kind::Option: {
        const auto& prompts = chat->conversation.questions.front().params.at("questions");
        const auto& options = prompts.at(chat->question_index).at("options");
        respond_chat_question(options.at(hit.index).value("label", ""));
        break;
    }
    }
    flush_agent();
}

bool Application::chat_key(const XEvent& event)
{
    auto key_event = event.xkey;
    const auto key = XLookupKeysym(&key_event, 0);
    if (key == XK_Tab && !(event.xkey.state & ControlMask)) {
        focus_chat();
        return true;
    }
    if ((key == XK_l || key == XK_L) && (event.xkey.state & ControlMask)) {
        focus_chat();
        return true;
    }
    if (!chat || !chat->focused)
        return false;
    if (key == XK_Escape) {
        if (chat->composing)
            close_agent_window();
        chat->focused = false;
        return true;
    }
    if (chat_shortcut(key, event.xkey.state, event)) {
        schedule_save();
        redraw = true;
        return true;
    }
    if (event.xkey.state & (ControlMask | Mod1Mask))
        return true;
    KeySym symbol;
    Status status = 0;
    std::string text(256, '\0');
    int length = input_context ? Xutf8LookupString(input_context, &key_event, text.data(),
                                                   static_cast<int>(text.size()), &symbol, &status)
                               : XLookupString(&key_event, text.data(),
                                               static_cast<int>(text.size()), &symbol, nullptr);
    if (status == XBufferOverflow) {
        text.resize(static_cast<std::size_t>(length));
        length =
            Xutf8LookupString(input_context, &key_event, text.data(), length, &symbol, &status);
    }
    if (length > 0) {
        text.resize(static_cast<std::size_t>(length));
        if (g_utf8_validate(text.c_str(), static_cast<gssize>(text.size()), nullptr))
            chat->input.insert(text);
    }
    schedule_save();
    redraw = true;
    return true;
}

bool Application::chat_shortcut(KeySym key, unsigned state, const XEvent& event)
{
    auto& input = chat->input;
    const bool shift = state & ShiftMask;
    if (state & ControlMask) {
        switch (key) {
        case XK_a:
        case XK_A:
            input.select_all();
            return true;
        case XK_v:
        case XK_V:
            XConvertSelection(display, XInternAtom(display, "CLIPBOARD", False),
                              XInternAtom(display, "UTF8_STRING", False),
                              XInternAtom(display, "SOURCE_CANVAS_PASTE", False), window,
                              event.xkey.time);
            return true;
        case XK_c:
        case XK_C:
        case XK_x:
        case XK_X:
            clipboard_text = input.selected();
            XSetSelectionOwner(display, XInternAtom(display, "CLIPBOARD", False), window,
                               event.xkey.time);
            if (key == XK_x || key == XK_X)
                input.insert("");
            return true;
        default:
            return false;
        }
    }
    switch (key) {
    case XK_Return:
    case XK_KP_Enter:
        if (shift)
            input.insert("\n");
        else {
            chat_action({ChatHit::Kind::Send, {}}, {});
            chat->follow = true;
        }
        return true;
    case XK_BackSpace:
        input.erase(true);
        return true;
    case XK_Delete:
        input.erase(false);
        return true;
    case XK_Left:
    case XK_Right:
        input.move(key == XK_Left ? -1 : 1, shift);
        return true;
    case XK_Up:
    case XK_Down:
        move_chat_caret(key == XK_Up ? -1 : 1, shift);
        return true;
    case XK_Home:
    case XK_End:
        input.line_end(key == XK_End, shift);
        return true;
    default:
        return false;
    }
}

void Application::chat_selection(const XEvent& event)
{
    if (event.type == SelectionNotify && chat && event.xselection.property != None) {
        Atom type;
        int format;
        unsigned long count = 0, remaining = 0;
        unsigned char* bytes = nullptr;
        if (XGetWindowProperty(display, window, event.xselection.property, 0, 262144, True,
                               AnyPropertyType, &type, &format, &count, &remaining,
                               &bytes) == Success &&
            bytes) {
            if (format == 8 && g_utf8_validate(reinterpret_cast<char*>(bytes),
                                               static_cast<gssize>(count), nullptr))
                chat->input.insert(std::string(reinterpret_cast<char*>(bytes), count));
            XFree(bytes);
        }
        redraw = true;
        schedule_save();
    } else if (event.type == SelectionRequest) {
        const auto& request = event.xselectionrequest;
        XEvent response{};
        response.xselection = {
            SelectionNotify, 0,    True,        display, request.requestor, request.selection,
            request.target,  None, request.time};
        const Atom property = request.property == None ? request.target : request.property;
        const Atom utf8 = XInternAtom(display, "UTF8_STRING", False);
        if (request.target == utf8 || request.target == XA_STRING) {
            const auto& text = request.selection == XA_PRIMARY ? primary_text : clipboard_text;
            XChangeProperty(display, request.requestor, property, request.target, 8,
                            PropModeReplace, reinterpret_cast<const unsigned char*>(text.data()),
                            static_cast<int>(text.size()));
            response.xselection.property = property;
        } else if (request.target == XInternAtom(display, "TARGETS", False)) {
            const Atom targets[]{utf8, XA_STRING};
            XChangeProperty(display, request.requestor, property, XA_ATOM, 32, PropModeReplace,
                            reinterpret_cast<const unsigned char*>(targets), 2);
            response.xselection.property = property;
        }
        XSendEvent(display, request.requestor, False, 0, &response);
    }
}

bool Application::chat_over_text(Vec2 pointer)
{
    if (!chat || contains(chat->header_bounds, pointer))
        return false;
    const auto world = camera.screen_to_world(pointer, viewport());
    if (contains(chat->editor, world))
        return true;
    for (auto hit = chat->hits.rbegin(); hit != chat->hits.rend(); ++hit) {
        if (!contains(hit->bounds, world))
            continue;
        PangoLayout* layout = hit->text_layout;
        if (hit->kind == ChatHit::Kind::Message) {
            if (!contains(chat->body, world) || hit->index >= chat->conversation.entries.size())
                continue;
            const auto& entry = chat->conversation.entries[hit->index];
            const auto found = chat->messages.find(entry.id);
            if (found != chat->messages.end())
                layout = found->second.layout.get();
        }
        if (!layout)
            return false;
        int index = 0, trailing = 0;
        return pango_layout_xy_to_index(
            layout, static_cast<int>((world.x - hit->bounds.origin.x) * PANGO_SCALE),
            static_cast<int>((world.y - hit->bounds.origin.y) * PANGO_SCALE), &index, &trailing);
    }
    return false;
}

bool Application::open_chat_link(const DocumentView& view, Vec2 point)
{
    int index = 0, trailing = 0;
    if (!pango_layout_xy_to_index(view.layout.get(), static_cast<int>(point.x * PANGO_SCALE),
                                  static_cast<int>(point.y * PANGO_SCALE), &index, &trailing))
        return false;
    for (const auto& span : view.document.styles) {
        if (span.style != PopupStyle::Link || index < static_cast<int>(span.begin) ||
            index >= static_cast<int>(span.begin + span.length))
            continue;
        return open_link(span.destination);
    }
    return false;
}

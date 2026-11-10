#include "application.h"
#include <X11/Xutil.h>

void Application::show_shortcuts()
{
    hide_popup();
    show_popup("# Shortcuts\n\n"
               "## Search and help\n"
               "`Ctrl+O` — choose a project directory\n"
               "`/` — search file names and text across the project\n"
               "`?` or `F1` — this guide\n"
               "`↑ / ↓` or `Shift+Tab / Tab` — choose a search result\n"
               "`Page Up / Down` — move through search results\n"
               "`Enter` — jump to the selected result\n"
               "`Esc` — close search or this guide\n"
               "In this guide: arrows, Page Up/Down, Home/End or wheel scroll.\n\n"
               "## Canvas and camera\n"
               "`Wheel` — smooth zoom at the pointer\n"
               "`+ / = / −` — zoom at the viewport center\n"
               "`Arrow keys` — pan\n"
               "`Alt+Left / Alt+Right` — camera back / forward\n"
               "`Mouse back / forward buttons` — camera back / forward\n"
               "`Page Up / Down` — pan by a screen\n"
               "`Home / End` — top / bottom of the canvas\n"
               "`0` — reset camera\n"
               "`F` — fit everything · `W` — fit width\n"
               "`L` — arrange files with Graphviz\n"
               "`G` — toggle dependency arrows\n"
               "Drag blank space or middle-drag on source or blank canvas to pan.\n"
               "Middle-click a conversation, intent card, search or popup to close it.\n"
               "Drag a file, agent or decision title to move its card.\n"
               "Wheel zoom continues while panning.\n\n"
               "## Code and navigation\n"
               "`Hover` or `K` — documentation for the pointed symbol\n"
               "`Ctrl+click / F12` — definition; at a definition, Ctrl+click shows usages\n"
               "`Shift+F12` — usages\n"
               "`Ctrl+Space` — completion suggestions\n"
               "`S` or click a file title — symbol hierarchy\n"
               "`C / Shift+C` — collapse / expand blocks in the pointed file\n"
               "Without a pointed file, C / Shift+C applies to all files.\n"
               "Hover a block header and click ▸/▾ to fold it.\n"
               "Click a file link or a navigation result to jump.\n\n"
               "## Agent conversations\n"
               "`Right-click / Ctrl+Shift+A` — type a message here to start a conversation\n"
               "`Tab / Ctrl+L` — focus and fit the active conversation\n"
               "`Enter` — send, reply or steer the active turn\n"
               "`Shift+Enter` — newline\n"
               "`Esc` — dismiss a new input or leave the message editor\n"
               "Drag the agent scrollbar to scroll; wheel still zooms the canvas.\n"
               "Use Conversations to switch threads.\n\n"
               "## Decisions and intent\n"
               "`D` — focus decisions\n"
               "`I / Alt+click source` — intent and diff for that line\n"
               "`Alt+click a human message` — select its intent\n"
               "`Alt+click an agent response` — record its public explanation as rationale\n"
               "In a rationale editor: Enter saves, Esc cancels, arrows move the caret.\n\n"
               "## Text selection and editing\n"
               "Drag text to select; Shift+click extends a selection.\n"
               "`Ctrl+A / Ctrl+C` — select all / copy the active text\n"
               "`Ctrl+X / Ctrl+V` — cut / paste in message and search editors\n"
               "`← / →` — move the caret; hold Shift to select\n"
               "`↑ / ↓` — move between message editor lines\n"
               "`Home / End` — start / end of the line; Shift extends\n"
               "`Backspace / Delete` — delete before / after the caret\n"
               "`Esc` — clear selection or cancel a text / scrollbar drag\n\n"
               "Canvas letter shortcuts accept uppercase too. / and ? type normally in editors.\n"
               "Keypad 0, +, − and Enter work like their main keyboard keys.",
               {std::max(0.0, (width - 660.0) / 2), 0}, true);
    shortcuts_visible = true;
}
bool Application::shortcuts_key(KeySym key)
{
    switch (key) {
    case XK_Escape:
        hide_popup();
        break;
    case XK_Up:
        popup_scroll -= 60;
        break;
    case XK_Down:
        popup_scroll += 60;
        break;
    case XK_Page_Up:
        popup_scroll -= std::max(60, height - 80);
        break;
    case XK_Page_Down:
        popup_scroll += std::max(60, height - 80);
        break;
    case XK_Home:
        popup_scroll = 0;
        break;
    case XK_End:
        popup_scroll = 1e9;
        break;
    default:
        break;
    }
    return true;
}
bool Application::canvas_shortcut_key(const XEvent& event)
{
    if (navigation_key(event))
        return true;
    auto key_event = event.xkey;
    const auto symbol = XLookupKeysym(&key_event, 0);
    if ((event.xkey.state & ControlMask) && !(event.xkey.state & Mod1Mask) &&
        (symbol == XK_o || symbol == XK_O)) {
        open_directory_picker();
        return true;
    }
    if (search.active)
        return search_key(event);
    KeySym key;
    char text[8]{};
    const auto length = XLookupString(&key_event, text, sizeof(text), &key, nullptr);
    if (key == XK_F1) {
        show_shortcuts();
        return true;
    }
    const bool editing = (chat && chat->focused) || (intent && intent->editing);
    if (!editing && !drag_button && !(event.xkey.state & (ControlMask | Mod1Mask)) && length == 1) {
        if (text[0] == '/') {
            open_search();
            return true;
        }
        if (text[0] == '?') {
            if (shortcuts_visible)
                hide_popup();
            else
                show_shortcuts();
            return true;
        }
    }
    if (shortcuts_visible) {
        if (event.xkey.state & ControlMask)
            return selection_key(event);
        return shortcuts_key(key);
    }
    return false;
}

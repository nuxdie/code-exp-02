#include "application.h"
#include <X11/Xutil.h>

void Application::show_shortcuts()
{
    hide_popup();
    show_popup(
        "# Keyboard shortcuts\n"
        "Find your way around the canvas.\n"
        "Scroll to explore   •   Esc to close\n\n"
        "## Search and help\n"
        "`Ctrl+O`\tchoose a project directory\n"
        "`/`\tsearch file names and text across the project\n"
        "`?` or `F1`\tthis guide\n"
        "`↑ / ↓` or `Shift+Tab / Tab`\tchoose a search result\n"
        "`Page Up / Down`\tmove through search results\n"
        "`Enter`\tjump to the selected result\n"
        "`Esc`\tclose focused UI; on the canvas, open Options and Settings\n"
        "In this guide: arrows, Page Up/Down, Home/End or wheel scroll.\n\n"
        "## Canvas and camera\n"
        "`Wheel`\tsmooth zoom at the pointer\n"
        "`+ / = / −`\tzoom at the viewport center\n"
        "`Arrow keys`\tpan\n"
        "`Alt+Left / Alt+Right`\tcamera back / forward\n"
        "`Mouse back / forward buttons`\tcamera back / forward\n"
        "`Page Up / Down`\tpan by a screen\n"
        "`Home / End`\ttop / bottom of the canvas\n"
        "`0`\treset camera\n"
        "`F / W`\tfit everything / fit width\n"
        "`L`\tarrange files by folder\n"
        "`G`\thide or show arrows\n"
        "Drag blank space or middle-drag on source or blank canvas to pan.\n"
        "Middle-click a conversation, note, search or popup to close it.\n"
        "Drag a file or agent title to move its card.\n"
        "Wheel zoom continues while panning.\n\n"
        "## Code and navigation\n"
        "`Hover` or `K`\tdocumentation for the pointed symbol\n"
        "`Click`\tarrows to where a symbol is defined and used; click a marker to fly there\n"
        "`F12`\tgo to definition\n"
        "`Shift+F12`\tusages\n"
        "`Ctrl+Space`\tcompletion suggestions\n"
        "`S / click file title`\tsymbol hierarchy\n"
        "`C / Shift+C`\tcollapse / expand blocks in the pointed file\n"
        "Without a pointed file, C / Shift+C applies to all files.\n"
        "Hover a block header and click ▸/▾ to fold it.\n"
        "Click a file link or a navigation result to jump.\n\n"
        "## Agent conversations\n"
        "`Right-click / Ctrl+Shift+A`\ttype a message here to start a conversation\n"
        "`Tab / Ctrl+L`\tfocus and fit the active conversation\n"
        "`Enter`\tsend, reply or steer the active turn\n"
        "`Shift+Enter`\tnewline\n"
        "`Esc`\tdismiss a new input or leave the message editor\n"
        "Drag the agent scrollbar to scroll; wheel still zooms the canvas.\n"
        "Use Conversations to switch threads.\n\n"
        "## Intent\n"
        "A coloured bar beside code marks the decision that produced it.\n"
        "`I / Alt+click / click the bar`\tyour words and the agent's why; lights every line "
        "it produced\n"
        "`Esc / middle-click the note`\tclear\n\n"
        "## Text selection and editing\n"
        "Drag text to select; Shift+click extends a selection.\n"
        "`Ctrl+A / Ctrl+C`\tselect all / copy the active text\n"
        "`Ctrl+X / Ctrl+V`\tcut / paste in message and search editors\n"
        "`← / →`\tmove the caret; hold Shift to select\n"
        "`↑ / ↓`\tmove between message editor lines\n"
        "`Home / End`\tstart / end of the line; Shift extends\n"
        "`Backspace / Delete`\tdelete before / after the caret\n"
        "`Esc`\tclear selection or cancel a text / scrollbar drag\n\n"
        "Canvas letter shortcuts accept uppercase too. / and ? type normally in editors.\n"
        "Keypad 0, +, − and Enter work like their main keyboard keys.",
        {std::max(0.0, (width - 868.0) / 2), 12}, true);
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
    auto options_event = event.xkey;
    if (options_page != OptionsPage::Closed)
        return options_key(XLookupKeysym(&options_event, 0));
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
    const bool editing = chat && chat->focused;
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

void Application::show_options(bool settings)
{
    text_selection = {};
    hide_popup();
    std::string text = "Options\nSettings [Enter / S]\nKeyboard shortcuts [? / F1]\nClose [Esc]";
    if (settings)
        text = "Settings · Appearance\n" + std::string(appearance == "system" ? "● " : "○ ") +
               "System [1]\n" + (appearance == "light" ? "● " : "○ ") + "Light [2]\n" +
               (appearance == "dark" ? "● " : "○ ") + "Dark [3]\n\nMonospace font\n" +
               (monospace_font == MonospaceFont::JetBrains ? "● " : "○ ") + "JetBrains Mono [4]\n" +
               (monospace_font == MonospaceFont::Victor ? "● " : "○ ") +
               "Victor Mono [5]\nBack [Esc]";
    show_popup(text, {std::max(0.0, (width - 400.0) / 2), std::max(0.0, (height - 240.0) / 2)});
    options_page = settings ? OptionsPage::Settings : OptionsPage::Options;
}

void Application::options_action(int row)
{
    if (options_page == OptionsPage::Options) {
        if (row == 1)
            show_options(true);
        else if (row == 2)
            show_shortcuts();
        else if (row == 3)
            hide_popup();
    } else if (options_page == OptionsPage::Settings) {
        if (row >= 1 && row <= 3) {
            appearance = row == 1 ? "system" : row == 2 ? "light" : "dark";
            renderer->set_dark_theme(appearance == "system" ? desktop_dark : appearance == "dark");
            schedule_save();
            show_options(true);
        } else if (row == 6 || row == 7) {
            set_monospace_font(row == 6 ? MonospaceFont::JetBrains : MonospaceFont::Victor);
            schedule_save();
            show_options(true);
        } else if (row == 8)
            show_options();
    }
}

bool Application::options_key(KeySym key)
{
    if (key == XK_Escape) {
        if (options_page == OptionsPage::Settings)
            show_options();
        else
            hide_popup();
    } else if (options_page == OptionsPage::Options) {
        if (key == XK_Return || key == XK_KP_Enter || key == XK_s || key == XK_S)
            options_action(1);
        else if (key == XK_F1 || key == XK_question || key == XK_slash)
            options_action(2);
    } else if (key >= XK_1 && key <= XK_3)
        options_action(static_cast<int>(key - XK_1) + 1);
    else if (key == XK_4 || key == XK_5)
        options_action(key == XK_4 ? 6 : 7);
    return true;
}

void Application::style_shortcuts(int layout_width)
{
    // Keep byte offsets stable for selection while stacking key/action pairs on small windows.
    if (layout_width < 600) {
        auto stacked = popup_text;
        std::replace(stacked.begin(), stacked.end(), '\t', '\n');
        pango_layout_set_text(popup_layout, stacked.c_str(), static_cast<int>(stacked.size()));
    }
    // The guide is a reference sheet: UI text for actions, monospace for physical keys.
    auto* font = pango_font_description_from_string("sans-serif");
    pango_font_description_set_absolute_size(font, 14 * PANGO_SCALE);
    pango_layout_set_font_description(popup_layout, font);
    pango_font_description_free(font);
    auto* tabs = pango_tab_array_new(1, TRUE);
    pango_tab_array_set_tab(tabs, 0, PANGO_TAB_LEFT, std::min(280, layout_width / 2));
    pango_layout_set_tabs(popup_layout, tabs);
    pango_tab_array_free(tabs);
    pango_layout_set_spacing(popup_layout, 8 * PANGO_SCALE);
    auto* attributes = pango_attr_list_new();
    auto add = [&](PangoAttribute* attribute, std::size_t begin, std::size_t length) {
        attribute->start_index = static_cast<guint>(begin);
        attribute->end_index = static_cast<guint>(begin + length);
        pango_attr_list_insert(attributes, attribute);
    };
    add(pango_attr_foreground_new(0x2900, 0x3500, 0x4500), 0, popup_text.size());
    for (const auto& span : popup_styles) {
        if (span.style != PopupStyle::Heading)
            continue;
        add(pango_attr_size_new_absolute((span.begin == 0 ? 28 : 18) * PANGO_SCALE), span.begin,
            span.length);
        add(pango_attr_weight_new(PANGO_WEIGHT_BOLD), span.begin, span.length);
        add(pango_attr_foreground_new(0x2300, 0x4400, 0x6a00), span.begin, span.length);
    }
    for (const auto& [begin, length] : popup_inline_code) {
        add(pango_attr_family_new(monospace_family()), begin, length);
        add(pango_attr_size_new_absolute(12 * PANGO_SCALE), begin, length);
        add(pango_attr_weight_new(PANGO_WEIGHT_MEDIUM), begin, length);
    }
    pango_layout_set_attributes(popup_layout, attributes);
    pango_attr_list_unref(attributes);
}

void Application::draw_shortcut_keys(Vec2 origin)
{
    for (const auto& [begin, length] : popup_inline_code) {
        PangoRectangle first{}, last{};
        pango_layout_index_to_pos(popup_layout, static_cast<int>(begin), &first);
        pango_layout_index_to_pos(popup_layout, static_cast<int>(begin + length), &last);
        // A key label may wrap in a small window. Never draw across unrelated rows.
        if (first.y != last.y || last.x <= first.x)
            continue;
        const double x = origin.x + static_cast<double>(first.x) / PANGO_SCALE;
        const double y = origin.y + static_cast<double>(first.y) / PANGO_SCALE;
        renderer->rectangle({{x - 3, y - 2},
                             {static_cast<double>(last.x - first.x) / PANGO_SCALE + 6,
                              static_cast<double>(first.height) / PANGO_SCALE + 4}},
                            {0.89f, 0.92f, 0.96f, 1});
    }
}

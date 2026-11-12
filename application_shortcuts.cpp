#include "application.h"
#include <X11/Xutil.h>

void Application::show_shortcuts()
{
    hide_popup();
    show_popup(
        "# Keyboard shortcuts\n\n"
        "## Canvas\n"
        "`Wheel`\tzoom at the pointer\n"
        "`Drag`\tpan; drag a title to move its card\n"
        "`F`\tfit everything\n"
        "`Alt+Left / Alt+Right`\tcamera back / forward\n"
        "`/`\tsearch file names and text\n"
        "`Ctrl+O`\topen another project\n"
        "`Middle-click`\tclose a note, search or conversation\n\n"
        "## Code\n"
        "`Hover`\tdocumentation\n"
        "`Click`\tarrows to where a symbol is defined and used; click a marker to fly there\n"
        "`C / Shift+C`\tcollapse / expand blocks\n"
        "`I / Alt+click / click the bar`\tthe decision behind a line\n\n"
        "## Conversations\n"
        "`Right-click / Ctrl+Shift+A`\tstart a conversation here\n"
        "`Tab / Ctrl+L`\tfocus the active conversation\n"
        "`Enter / Shift+Enter`\tsend / newline\n"
        "A closed conversation stays as its title: click to reopen, middle-click to remove.\n\n"
        "## Text\n"
        "Drag to select; Shift+click extends.\n"
        "`Ctrl+A / Ctrl+C / Ctrl+X / Ctrl+V`\tselect all / copy / cut / paste\n"
        "`Esc`\tclose or cancel",
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

void Application::style_shortcuts(int layout_width)
{
    // Keep byte offsets stable for selection while stacking key/action pairs on small windows.
    if (layout_width < 600) {
        auto stacked = popup_text;
        std::ranges::replace(stacked, '\t', '\n');
        pango_layout_set_text(popup_layout.get(), stacked.c_str(),
                              static_cast<int>(stacked.size()));
    }
    // The guide is a reference sheet: UI text for actions, monospace for physical keys.
    pango_layout_set_font_description(popup_layout.get(), font_description("sans-serif", 14).get());
    const TabsHandle tabs{pango_tab_array_new(1, TRUE)};
    pango_tab_array_set_tab(tabs.get(), 0, PANGO_TAB_LEFT, std::min(280, layout_width / 2));
    pango_layout_set_tabs(popup_layout.get(), tabs.get());
    pango_layout_set_spacing(popup_layout.get(), 8 * PANGO_SCALE);
    const AttributesHandle attributes{pango_attr_list_new()};
    auto add = [&](PangoAttribute* attribute, std::size_t begin, std::size_t length) {
        attribute->start_index = static_cast<guint>(begin);
        attribute->end_index = static_cast<guint>(begin + length);
        pango_attr_list_insert(attributes.get(), attribute);
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
        add(pango_attr_family_new(monospace_family), begin, length);
        add(pango_attr_size_new_absolute(12 * PANGO_SCALE), begin, length);
        add(pango_attr_weight_new(PANGO_WEIGHT_MEDIUM), begin, length);
    }
    pango_layout_set_attributes(popup_layout.get(), attributes.get());
}

void Application::draw_shortcut_keys(Vec2 origin)
{
    for (const auto& [begin, length] : popup_inline_code) {
        PangoRectangle first{}, last{};
        pango_layout_index_to_pos(popup_layout.get(), static_cast<int>(begin), &first);
        pango_layout_index_to_pos(popup_layout.get(), static_cast<int>(begin + length), &last);
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

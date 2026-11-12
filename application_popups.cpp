#include "application.h"
#include "popup_markup.h"

void Application::hide_popup()
{
    shortcuts_visible = false;
    if (!popup_text.empty())
        redraw = true;
    popup_text.clear();
    hover_request = 0;
    hover_due = Clock::time_point::max();
    popup_dismiss_due = Clock::time_point::max();
}

void Application::show_popup(std::string text, Vec2 origin, bool syntax)
{
    shortcuts_visible = false;
    auto document = popup_document(text, syntax);
    popup_text = std::move(document.text);
    popup_code_lines = std::move(document.code_lines);
    popup_inline_code = std::move(document.inline_code);
    popup_prose = syntax;
    popup_styles = std::move(document.styles);
    popup_scroll = 0;
    cached_popup.clear();
    popup_origin = origin;
    popup_dismiss_due = Clock::time_point::max();
    redraw = true;
}

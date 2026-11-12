#include "application.h"

namespace {
constexpr std::size_t match_limit = 200;
void make_preview(SearchMatch& match, const std::string& line)
{
    auto first = match.byte > 80 ? match.byte - 80 : 0;
    while (first < line.size() && (static_cast<unsigned char>(line[first]) & 0xc0) == 0x80)
        ++first;
    auto last = std::min(line.size(), std::max(first + 240, match.byte + match.length));
    while (last < line.size() && (static_cast<unsigned char>(line[last]) & 0xc0) == 0x80)
        --last;
    const std::string prefix = first ? "… " : "";
    match.preview = prefix + line.substr(first, last - first) + (last < line.size() ? " …" : "");
    match.preview_byte = prefix.size() + match.byte - first;
}
void source_matches(SearchCanvas& search, const SourcePanel& panel, GRegex* pattern)
{
    for (std::size_t line = 0; line < panel.text.size() && search.matches.size() < match_limit;
         ++line) {
        GMatchInfo* raw = nullptr;
        g_regex_match(pattern, panel.text[line].c_str(), G_REGEX_MATCH_DEFAULT, &raw);
        const Handle<GMatchInfo, g_match_info_free> info{raw};
        while (g_match_info_matches(info.get()) && search.matches.size() < match_limit) {
            int first = 0, last = 0;
            g_match_info_fetch_pos(info.get(), 0, &first, &last);
            SearchMatch match;
            match.path = panel.path;
            match.uri = panel.uri;
            match.line = static_cast<int>(line);
            match.byte = static_cast<std::size_t>(first);
            match.length = static_cast<std::size_t>(last - first);
            match.cmake = panel.cmake;
            make_preview(match, panel.text[line]);
            search.matches.push_back(std::move(match));
            g_match_info_next(info.get(), nullptr);
        }
    }
}
} // namespace
void Application::open_search()
{
    hide_popup();
    text_selection = {};
    search.active = true;
    search.query.select_all();
    update_search();
    redraw = true;
}
void Application::close_search()
{
    if (text_selection.surface == TextSurface::Search) {
        if (text_selection.dragging) {
            XUngrabPointer(display, CurrentTime);
            drag_button = 0;
        }
        text_selection = {};
    }
    search.active = false;
    schedule_save();
    redraw = true;
}
void Application::update_search()
{
    schedule_save();
    for (auto& match : search.matches) {
        renderer->release(match.label.mesh);
        renderer->release(match.snippet.mesh);
    }
    search.matches.clear();
    search.hits.clear();
    search.selected = search.offset = 0;
    search.more = false;
    if (search.query.text.empty())
        return;
    const GStringHandle literal{g_regex_escape_string(search.query.text.c_str(), -1)};
    const Handle<GRegex, g_regex_unref> pattern{g_regex_new(
        literal.get(), static_cast<GRegexCompileFlags>(G_REGEX_CASELESS | G_REGEX_OPTIMIZE),
        G_REGEX_MATCH_DEFAULT, nullptr)};
    if (!pattern)
        return;
    for (const auto& panel : panels) {
        if (search.matches.size() >= match_limit)
            break;
        if (!g_regex_match(pattern.get(), panel.path.c_str(), G_REGEX_MATCH_DEFAULT, nullptr))
            continue;
        SearchMatch match;
        match.path = panel.path;
        match.uri = panel.uri;
        match.filename = true;
        match.preview = "File name";
        search.matches.push_back(std::move(match));
    }
    for (const auto& panel : panels) {
        source_matches(search, panel, pattern.get());
        if (search.matches.size() >= match_limit) {
            search.more = true;
            break;
        }
    }
    redraw = true;
}
void Application::move_search(int direction)
{
    if (search.matches.empty())
        return;
    search.selected =
        static_cast<std::size_t>(std::clamp(static_cast<int>(search.selected) + direction, 0,
                                            static_cast<int>(search.matches.size()) - 1));
    redraw = true;
}
void Application::open_search_match(std::size_t index)
{
    if (index >= search.matches.size())
        return;
    const auto& match = search.matches[index];
    const auto panel = std::ranges::find(panels, match.path, &SourcePanel::path);
    if (panel == panels.end())
        return;
    if (panel->text.empty()) {
        close_search();
        remember_view();
        camera.center = panel->bounds.origin;
        camera.zoom = 1;
        zoom_animation.cancel(camera);
        finish_view();
        return;
    }
    const auto line = std::min(match.line, std::max(0, static_cast<int>(panel->text.size()) - 1));
    const int column = panel->text.empty() ? 0 : lsp::byte_to_utf16(panel->text[line], match.byte);
    const lsp::Json location{{"uri", panel->uri},
                             {"range", {{"start", {{"line", line}, {"character", column}}}}}};
    close_search();
    jump_to(location);
}

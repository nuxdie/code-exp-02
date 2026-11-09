#include "application.h"
#include <cstring>

namespace {
bool contains(const CanvasRect& bounds, Vec2 point)
{
    return Viewport{bounds.origin.x, bounds.origin.y, bounds.size.x, bounds.size.y}.contains(point);
}
} // namespace
TextTarget Application::chat_text_target(Vec2 pointer)
{
    const auto world = camera.screen_to_world(pointer, viewport());
    if (!chat || !contains(chat->bounds, world))
        return {};
    if (contains(chat->editor, world) && chat->composer.layout)
        return {TextSurface::Composer,
                "",
                &chat->input.text,
                chat->composer.layout.get(),
                nullptr,
                camera.world_to_screen(
                    {chat->editor.origin.x + 12, chat->editor.origin.y + 10 - chat->editor_scroll},
                    viewport()),
                camera.zoom};
    if (!contains(chat->body, world))
        return {};
    for (const auto& hit : chat->hits) {
        if (hit.kind != ChatHit::Kind::Message || !contains(hit.bounds, world) ||
            hit.index >= chat->conversation.entries.size())
            continue;
        const auto& id = chat->conversation.entries[hit.index].id;
        const auto found = chat->messages.find(id);
        if (found != chat->messages.end()) {
            const auto& view = found->second;
            return {TextSurface::Chat,   id,
                    &view.document.text, view.layout.get(),
                    &view.document.math, camera.world_to_screen(hit.bounds.origin, viewport()),
                    camera.zoom};
        }
    }
    return {};
}
TextTarget Application::source_text_target(Vec2 pointer)
{
    const auto world = camera.screen_to_world(pointer, viewport());
    for (const auto& header : header_hits)
        if (contains(header, pointer))
            return {};
    for (const auto& panel : panels) {
        if (world.y < panel.bounds.origin.y + header_height || !contains(panel.bounds, world))
            continue;
        const auto row =
            static_cast<int>((world.y - panel.bounds.origin.y - header_height) / line_height);
        if (row < 0 || row >= static_cast<int>(panel.folded.source_lines.size()))
            continue;
        const auto line = panel.folded.source_lines[row];
        int index = 0, trailing = 0;
        if (!pango_layout_xy_to_index(
                panel.lines[line],
                static_cast<int>((world.x - panel.bounds.origin.x - 24) * PANGO_SCALE),
                font_height * PANGO_SCALE / 2, &index, &trailing))
            continue;
        return {TextSurface::Source, panel.path, &panel.source};
    }
    return {};
}
TextTarget Application::text_at(Vec2 pointer)
{
    if (!popup_text.empty() && contains(popup_bounds, pointer))
        return {TextSurface::Popup,
                "",
                &popup_text,
                popup_layout,
                &popup_math,
                {popup_bounds.origin.x + 12, popup_bounds.origin.y + 12 - popup_scroll}};
    auto target = chat_text_target(pointer);
    if (target.surface != TextSurface::Inactive)
        return target;
    const auto world = camera.screen_to_world(pointer, viewport());
    if (chat && contains(chat->bounds, world))
        return {};
    if (intent && intent->placed && contains(intent->bounds, world)) {
        if (intent->editing && contains(intent->editor, world)) {
            const std::string prefix = "Human rationale · Enter saves · Esc cancels\n";
            return {TextSurface::Rationale,
                    "",
                    &intent->input.text,
                    intent->rationale.layout.get(),
                    nullptr,
                    camera.world_to_screen(
                        {intent->editor.origin.x + 12, intent->editor.origin.y + 10}, viewport()),
                    camera.zoom,
                    prefix.size()};
        }
        if (!intent->editing && contains(intent->body, world))
            return {
                TextSurface::Intent,
                "",
                &intent->document.document.text,
                intent->document.layout.get(),
                &intent->document.document.math,
                camera.world_to_screen(
                    {intent->body.origin.x, intent->body.origin.y - intent->scroll}, viewport()),
                camera.zoom};
        return {};
    }
    return source_text_target(pointer);
}
TextTarget Application::active_text_target()
{
    switch (text_selection.surface) {
    case TextSurface::Source:
        for (const auto& panel : panels)
            if (panel.path == text_selection.key)
                return {TextSurface::Source, panel.path, &panel.source};
        return {};
    case TextSurface::Chat: {
        double y = chat->body.origin.y - chat->scroll;
        for (std::size_t i = 0; i < chat->conversation.entries.size(); ++i) {
            const auto& entry = chat->conversation.entries[i];
            const auto found = chat->messages.find(entry.id);
            if (entry.id == text_selection.key && found != chat->messages.end()) {
                const auto& view = found->second;
                return {TextSurface::Chat,
                        entry.id,
                        &view.document.text,
                        view.layout.get(),
                        &view.document.math,
                        camera.world_to_screen({chat->body.origin.x, y}, viewport()),
                        camera.zoom};
            }
            y += chat_entry_height(i);
        }
        return {};
    }
    case TextSurface::Popup:
        if (!popup_text.empty())
            return {TextSurface::Popup,
                    "",
                    &popup_text,
                    popup_layout,
                    &popup_math,
                    {popup_bounds.origin.x + 12, popup_bounds.origin.y + 12 - popup_scroll}};
        return {};
    case TextSurface::Intent:
        return {TextSurface::Intent,
                "",
                &intent->document.document.text,
                intent->document.layout.get(),
                &intent->document.document.math,
                camera.world_to_screen(
                    {intent->body.origin.x, intent->body.origin.y - intent->scroll}, viewport()),
                camera.zoom};
    case TextSurface::Composer:
        return {TextSurface::Composer,
                "",
                &chat->input.text,
                chat->composer.layout.get(),
                nullptr,
                camera.world_to_screen(
                    {chat->editor.origin.x + 12, chat->editor.origin.y + 10 - chat->editor_scroll},
                    viewport()),
                camera.zoom};
    case TextSurface::Rationale: {
        const std::string prefix = "Human rationale · Enter saves · Esc cancels\n";
        return {TextSurface::Rationale,
                "",
                &intent->input.text,
                intent->rationale.layout.get(),
                nullptr,
                camera.world_to_screen({intent->editor.origin.x + 12, intent->editor.origin.y + 10},
                                       viewport()),
                camera.zoom,
                prefix.size()};
    }
    default:
        return {};
    }
}
std::size_t Application::source_byte_at(const SourcePanel& panel, Vec2 pointer)
{
    const auto world = camera.screen_to_world(pointer, viewport());
    if (panel.folded.source_lines.empty())
        return 0;
    const auto row =
        std::clamp(static_cast<int>(
                       std::floor((world.y - panel.bounds.origin.y - header_height) / line_height)),
                   0, static_cast<int>(panel.folded.source_lines.size()) - 1);
    const auto line = panel.folded.source_lines[row];
    int index = 0, trailing = 0;
    pango_layout_xy_to_index(panel.lines[line],
                             static_cast<int>((world.x - panel.bounds.origin.x - 24) * PANGO_SCALE),
                             font_height * PANGO_SCALE / 2, &index, &trailing);
    const auto& text = panel.text[line];
    const auto byte = std::min(text.size(), static_cast<std::size_t>(std::max(0, index)));
    const auto caret = static_cast<std::size_t>(
        g_utf8_offset_to_pointer(text.c_str() + byte, trailing) - text.c_str());
    std::size_t offset = 0;
    for (int i = 0; i < line; ++i)
        offset += panel.text[i].size() + 1;
    return offset + std::min(caret, text.size());
}
std::size_t Application::text_byte_at(const TextTarget& target, Vec2 pointer, bool* inside)
{
    if (target.surface == TextSurface::Source) {
        if (inside)
            *inside = true;
        for (const auto& panel : panels)
            if (panel.path == target.key)
                return source_byte_at(panel, pointer);
        return 0;
    }
    if (!target.layout || !target.text) {
        if (inside)
            *inside = false;
        return 0;
    }
    int index = 0, trailing = 0;
    const bool hit = pango_layout_xy_to_index(
        target.layout, static_cast<int>((pointer.x - target.origin.x) / target.scale * PANGO_SCALE),
        static_cast<int>((pointer.y - target.origin.y) / target.scale * PANGO_SCALE), &index,
        &trailing);
    if (inside)
        *inside = hit || target.surface == TextSurface::Composer ||
                  target.surface == TextSurface::Rationale;
    const auto* raw = pango_layout_get_text(target.layout);
    const auto byte = std::min(std::strlen(raw), static_cast<std::size_t>(std::max(0, index)));
    const auto caret =
        static_cast<std::size_t>(g_utf8_offset_to_pointer(raw + byte, trailing) - raw);
    return std::min(target.text->size(), caret < target.prefix ? 0 : caret - target.prefix);
}

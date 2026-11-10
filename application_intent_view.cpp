#include "application.h"
#include "intent_document.h"

void Application::draw_intent()
{
    if (!intent || !intent->placed || !intent->visible)
        return;
    anchor_intent();
    const auto origin = camera.world_to_screen(intent->bounds.origin, viewport());
    const CanvasRect screen{
        origin, {intent->bounds.size.x * camera.zoom, intent->bounds.size.y * camera.zoom}};
    if (!screen.intersects({{0, 0}, {static_cast<double>(width), static_cast<double>(height)}}))
        return;
    renderer->rectangle({{origin.x + 3, origin.y + 4}, screen.size}, {0.1f, 0.15f, 0.2f, 0.08f});
    renderer->rectangle(screen, {0.995f, 0.993f, 0.985f, 1});
    renderer->rectangle({origin, {screen.size.x, 4 * camera.zoom}}, {0.38f, 0.58f, 0.79f, 1});
    const auto title = intent->path.empty() ? "Intent history" : intent->path;
    const auto back = intent->path.empty() || intent->overview ? "" : "[File intents](file:)   ";
    update_document(intent->header,
                    "**" + intent_literal(intent_summary(title, 64)) + "**\n" + back +
                        "[All intents](all:)   [Conversation](conversation:)   [Refresh](refresh:)",
                    static_cast<int>(intent->bounds.size.x - 48));
    renderer->draw(intent->header.mesh,
                   camera.world_to_screen(
                       {intent->bounds.origin.x + 24, intent->bounds.origin.y + 16}, viewport()),
                   camera.zoom);
    if (camera.zoom < 0.25)
        return;
    intent->body = {
        {intent->bounds.origin.x + 24, intent->bounds.origin.y + 104},
        {intent->bounds.size.x - 48,
         intent->bounds.size.y - (intent->overview ? 144 : 210) - (intent->editing ? 160 : 0)}};
    auto source = intent_document(intent->records, intent->selected, intent->evidence,
                                  intent->overview, intent->path);
    if (!intent->error.empty())
        source = "**" + intent_literal(intent->error) + "**\n\n" + source;
    update_document(intent->document, source, static_cast<int>(intent->body.size.x));
    intent->scroll = std::clamp(intent->scroll, 0.0,
                                std::max(0.0, intent->document.size.y - intent->body.size.y));
    const auto body = camera.world_to_screen(intent->body.origin, viewport());
    renderer->clip({body, {intent->body.size.x * camera.zoom, intent->body.size.y * camera.zoom}},
                   height);
    if (text_selection.surface == TextSurface::Intent &&
        text_selection.range.text == intent->document.document.text)
        draw_selection_layout(intent->document.layout.get(),
                              {body.x, body.y - intent->scroll * camera.zoom}, camera.zoom,
                              text_selection.range.selection());
    renderer->draw(intent->document.mesh, {body.x, body.y - intent->scroll * camera.zoom},
                   camera.zoom);
    renderer->clear_clip();
    if (intent->document.size.y > intent->body.size.y) {
        const double thumb =
            std::max(28.0, intent->body.size.y * intent->body.size.y / intent->document.size.y);
        const double offset = (intent->body.size.y - thumb) * intent->scroll /
                              (intent->document.size.y - intent->body.size.y);
        const auto point = camera.world_to_screen(
            {intent->bounds.origin.x + intent->bounds.size.x - 10, intent->body.origin.y + offset},
            viewport());
        renderer->rectangle({point, {3 * camera.zoom, thumb * camera.zoom}},
                            {0.5f, 0.54f, 0.6f, 0.65f});
    }
    draw_intent_controls();
}
void Application::draw_intent_controls()
{
    if (intent->overview || intent->selected.empty()) {
        update_document(intent->controls, "Select an intent · I or Alt+click code for line history",
                        592, false);
        renderer->draw(
            intent->controls.mesh,
            camera.world_to_screen({intent->bounds.origin.x + 24,
                                    intent->bounds.origin.y + intent->bounds.size.y - 32},
                                   viewport()),
            camera.zoom);
        return;
    }
    const auto controls =
        intent->token.empty()
            ? "[Add explanation](why:)   [Review changes](preview:)   [Commit intent "
              "only](capture:)\nI or Alt+click source for line history"
            : "[Commit reviewed changes](checkpoint:)   [Cancel preview](cancel:)\nThe preview is "
              "frozen; later edits remain in the working tree.";
    update_document(intent->controls, controls, static_cast<int>(intent->bounds.size.x - 48));
    renderer->draw(intent->controls.mesh,
                   camera.world_to_screen({intent->bounds.origin.x + 24,
                                           intent->bounds.origin.y + intent->bounds.size.y - 86},
                                          viewport()),
                   camera.zoom);
    if (!intent->editing)
        return;
    intent->editor = {
        {intent->bounds.origin.x + 20, intent->bounds.origin.y + intent->bounds.size.y - 252},
        {intent->bounds.size.x - 40, 132}};
    const auto origin = camera.world_to_screen(intent->editor.origin, viewport());
    renderer->rectangle(
        {origin, {intent->editor.size.x * camera.zoom, intent->editor.size.y * camera.zoom}},
        {0.92f, 0.94f, 0.95f, 1});
    update_document(intent->rationale,
                    "Human rationale · Enter saves · Esc cancels\n" + intent->input.text + "│",
                    static_cast<int>(intent->editor.size.x - 24), false);
    renderer->clip(
        {origin, {intent->editor.size.x * camera.zoom, intent->editor.size.y * camera.zoom}},
        height);
    const auto range = intent->input.selection();
    const std::string prefix = "Human rationale · Enter saves · Esc cancels\n";
    draw_selection_layout(intent->rationale.layout.get(),
                          {origin.x + 12 * camera.zoom, origin.y + 10 * camera.zoom}, camera.zoom,
                          {range.first + prefix.size(), range.second + prefix.size()});
    renderer->draw(intent->rationale.mesh,
                   {origin.x + 12 * camera.zoom, origin.y + 10 * camera.zoom}, camera.zoom);
    renderer->clear_clip();
}

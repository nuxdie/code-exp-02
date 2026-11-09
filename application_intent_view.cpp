#include "application.h"
#include "intent_document.h"

void Application::draw_intent()
{
    if (!intent || !intent->placed || !intent->visible)
        return;
    const auto origin = camera.world_to_screen(intent->bounds.origin, viewport());
    const CanvasRect screen{
        origin, {intent->bounds.size.x * camera.zoom, intent->bounds.size.y * camera.zoom}};
    if (!screen.intersects({{0, 0}, {static_cast<double>(width), static_cast<double>(height)}}))
        return;
    renderer->rectangle(screen, {0.995f, 0.993f, 0.985f, 1});
    update_document(intent->header,
                    "**Decisions → evidence**\n[All decisions](all:)   "
                    "[Conversation](conversation:)   [Refresh](refresh:)",
                    760);
    renderer->draw(intent->header.mesh,
                   camera.world_to_screen(
                       {intent->bounds.origin.x + 24, intent->bounds.origin.y + 16}, viewport()),
                   camera.zoom);
    if (camera.zoom < 0.25)
        return;
    intent->body = {{intent->bounds.origin.x + 24, intent->bounds.origin.y + 104},
                    {intent->bounds.size.x - 48, intent->bounds.size.y - 210}};
    auto source =
        intent_document(intent->records, intent->selected, intent->evidence, intent->overview);
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
    draw_intent_controls();
}
void Application::draw_intent_controls()
{
    const auto controls =
        intent->token.empty()
            ? "[Record why](why:)   [Preview working changes](preview:)   [Checkpoint intent "
              "only](capture:)\nI or Alt+click on source → line history and intent"
            : "[Checkpoint reviewed diff](checkpoint:)   [Cancel preview](cancel:)\nThe preview is "
              "frozen; later edits remain in the working tree.";
    update_document(intent->controls, controls, 770);
    renderer->draw(intent->controls.mesh,
                   camera.world_to_screen({intent->bounds.origin.x + 24,
                                           intent->bounds.origin.y + intent->bounds.size.y - 86},
                                          viewport()),
                   camera.zoom);
    if (!intent->editing)
        return;
    intent->editor = {
        {intent->bounds.origin.x + 20, intent->bounds.origin.y + intent->bounds.size.y - 220},
        {intent->bounds.size.x - 40, 120}};
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

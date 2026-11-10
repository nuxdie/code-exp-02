#include "application.h"
#include "intent_document.h"

void Application::rebuild_intent_index()
{
    intent->file_records.clear();
    for (const auto& panel : panels)
        for (const auto& record : intent->records)
            if (intent_matches_path(record, panel.path))
                intent->file_records[panel.path].push_back(record.value("id", ""));
}

void Application::draw_intent_badges()
{
    if (!intent)
        return;
    intent->badge_hits.clear();
    if (!intent->ready || camera.zoom < 0.35)
        return;
    const double scale = std::max(0.7, camera.zoom);
    for (std::size_t i = 0; i < panels.size() && i < header_hits.size(); ++i) {
        const auto& panel = panels[i];
        const auto found = intent->file_records.find(panel.path);
        if (found == intent->file_records.end() || header_hits[i].size.x <= 0)
            continue;
        const auto& ids = found->second;
        const auto record =
            std::find_if(intent->records.rbegin(), intent->records.rend(),
                         [&](const auto& item) { return item["id"] == ids.back(); });
        if (record == intent->records.rend())
            continue;
        auto& badge = intent->badges[panel.path];
        const auto text = std::to_string(ids.size()) +
                          (ids.size() == 1 ? " intent · " : " intents · ") +
                          intent_summary(record->value("statement", ""), 90);
        update_document(badge, text, 360, false);
        if (pango_layout_get_ellipsize(badge.layout.get()) == PANGO_ELLIPSIZE_NONE) {
            pango_layout_set_height(badge.layout.get(), -1);
            pango_layout_set_ellipsize(badge.layout.get(), PANGO_ELLIPSIZE_END);
            upload_document_mesh(badge.mesh, badge.layout.get(), {});
        }
        const auto& title = header_hits[i];
        const CanvasRect bounds{{title.origin.x + title.size.x + 12 * scale, title.origin.y},
                                {384 * scale, 27 * scale}};
        const bool obscures_title =
            std::any_of(header_hits.begin(), header_hits.end(), [&](const auto& other) {
                return other.size.x > 0 && bounds.intersects(other);
            });
        if (obscures_title || !bounds.intersects({{0, 0}, {double(width), double(height)}}))
            continue;
        renderer->rectangle(bounds, {0.90f, 0.94f, 0.98f, 0.98f});
        renderer->draw(badge.mesh, {bounds.origin.x + 12 * scale, bounds.origin.y + 3 * scale},
                       scale);
        intent->badge_hits.push_back({bounds, panel.path});
    }
}

bool Application::intent_badge_button(const XEvent& event, Vec2 pointer)
{
    if (!intent || drag_button || event.xbutton.button != Button1 || chat_at(pointer))
        return false;
    for (const auto& badge : intent->badge_hits)
        if (Viewport{badge.bounds.origin.x, badge.bounds.origin.y, badge.bounds.size.x,
                     badge.bounds.size.y}
                .contains(pointer)) {
            show_file_intents(badge.path);
            return true;
        }
    return false;
}

void Application::show_file_intents(const std::string& path, double offset)
{
    if (!intent || !intent->placed)
        return;
    intent->path = path;
    const auto panel = std::find_if(panels.begin(), panels.end(),
                                    [&](const auto& source) { return source.path == path; });
    if (panel != panels.end()) {
        intent->bounds.origin = {panel->bounds.origin.x + panel->bounds.size.x + 24,
                                 panel->bounds.origin.y + offset};
        std::vector<CanvasRect> occupied;
        for (const auto& source : panels)
            occupied.push_back(source.bounds);
        intent->bounds = place_canvas_window(intent->bounds, occupied);
    }
    intent->overview = true;
    intent->editing = false;
    intent->evidence.clear();
    intent->token.clear();
    intent->scroll = 0;
    focus_intent();
}

void Application::select_intent(const std::string& id)
{
    intent->selected = id;
    if (intent->path.empty()) {
        for (const auto& record : intent->records) {
            if (record.value("id", "") != id)
                continue;
            for (const auto& panel : panels)
                if (intent_matches_path(record, panel.path)) {
                    show_file_intents(panel.path);
                    break;
                }
            break;
        }
    }
    intent->overview = false;
    intent->evidence.clear();
    intent->token.clear();
    intent->editing = false;
    intent->scroll = 0;
}

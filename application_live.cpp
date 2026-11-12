#include "application.h"

namespace {
void settle_panel(std::vector<SourcePanel>& panels, std::size_t index)
{
    auto& panel = panels[index];
    for (;;) {
        const auto end = panels.begin() + static_cast<std::ptrdiff_t>(index);
        const auto overlap = std::find_if(panels.begin(), end, [&](const auto& prior) {
            return panel.bounds.intersects(prior.bounds);
        });
        if (overlap == end)
            return;
        panel.bounds.origin.y = overlap->bounds.origin.y + overlap->bounds.size.y + 96;
    }
}
} // namespace

void Application::replace_source(int index, const std::string& source)
{
    auto& old = panels[index];
    auto updated = make_panel(old.path, source);
    for (auto& fold : updated.folds) {
        const auto key = fold_identity(updated, fold.first);
        const auto previous = std::ranges::find_if(
            old.folds, [&](const auto& other) { return fold_identity(old, other.first) == key; });
        if (previous != old.folds.end())
            fold.collapsed = previous->collapsed;
    }
    updated.bounds.origin = old.bounds.origin;
    updated.bounds.size.x += 32;
    if (old.image || updated.image)
        renderer->release(old.mesh);
    else
        updated.mesh = old.mesh;
    updated.header_mesh = old.header_mesh;
    updated.version = old.version;
    auto* server = server_for(old);
    if (server && opened_documents.contains(old.uri)) {
        ++updated.version;
        server->change({updated.uri, source});
    }
    old = std::move(updated);
    update_mesh(old);
}

void Application::apply_project_change(const ProjectChange& change)
{
    auto found = std::ranges::find(panels, change.path, &SourcePanel::path);
    if (!change.source) {
        if (found == panels.end())
            return;
        activity_deleted[change.path] = {found->bounds, Clock::now()};
        auto* server = server_for(*found);
        if (server && opened_documents.erase(found->uri))
            server->close(found->uri);
        renderer->release(found->mesh);
        renderer->release(found->header_mesh);
        panels.erase(found);
    } else if (found == panels.end()) {
        auto panel = make_panel(change.path, *change.source);
        update_mesh(panel);
        panels.push_back(std::move(panel));
        place_in_folder(panels.back());
    } else if (found->image || found->source != *change.source) {
        replace_source(static_cast<int>(found - panels.begin()), *change.source);
    }
}

void Application::poll_project()
{
    if (!project_watcher)
        return;
    const auto changes = project_watcher->take_changes();
    if (changes.empty())
        return;
    // A popup the pointer is resting in is being read; file changes elsewhere leave it alone.
    if (!shortcuts_visible && !CanvasWindowManager::contains(popup_bounds, mouse_position))
        hide_popup();
    const auto dragged =
        dragged_panel >= 0 && dragged_panel < std::ssize(panels) ? panels[dragged_panel].path : "";
    for (const auto& change : changes)
        apply_project_change(change);
    refresh_symbol_sources(changes);
    load_intent_margin();
    auto index_for = [&](const std::string& path) {
        const auto found = std::ranges::find(panels, path, &SourcePanel::path);
        return found == panels.end() ? -1 : static_cast<int>(found - panels.begin());
    };
    if (!dragged.empty()) {
        dragged_panel = index_for(dragged);
        if (dragged_panel < 0) {
            drag_button = 0;
            XUngrabPointer(display, CurrentTime);
        }
    }
    focused_line.reset();
    refresh_project_geometry();
    if (search.active)
        update_search();
    schedule_save();
    redraw = true;
}

void Application::refresh_project_geometry(bool settle)
{
    std::vector<GraphFile> files;
    file_sizes.clear();
    for (std::size_t i = 0; i < panels.size(); ++i) {
        if (settle)
            settle_panel(panels, i);
        const auto& panel = panels[i];
        files.push_back({panel.path, panel.source, panel.bounds.size});
        file_sizes.push_back(panel.bounds.size);
    }
    dependencies = file_dependencies(files);
    connections_dirty = true;
    graph.files.clear();
    for (const auto& panel : panels)
        graph.files.push_back(panel.bounds);
    auto bounds = conversation_bounds();
    bounds.insert(bounds.end(), graph.files.begin(), graph.files.end());
    content_size = canvas_size(bounds);
    connections_dirty = true;
}

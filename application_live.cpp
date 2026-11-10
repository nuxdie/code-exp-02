#include "application.h"

namespace {
bool needs_viewer_build(const std::string& path)
{
    if (path.compare(0, 6, "tests/") == 0)
        return false;
    if (path == "tools/agent_bridge.py" || path == "CMakeLists.txt" || path == ".clang-format" ||
        path == ".clang-tidy" || path == ".cmake-format.json" || path == "tools/cmake_style.py")
        return true;
    const auto extension = std::filesystem::path(path).extension().string();
    return extension == ".cpp" || extension == ".cc" || extension == ".cxx" || extension == ".c" ||
           extension == ".h" || extension == ".hpp" || extension == ".in" || extension == ".cmake";
}

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

void Application::release_panel(SourcePanel& panel)
{
    if (panel.title)
        g_object_unref(panel.title);
    for (auto* layout : panel.lines)
        g_object_unref(layout);
    for (auto* attributes : panel.lexical)
        pango_attr_list_unref(attributes);
    for (auto* label : panel.fold_labels)
        g_object_unref(label);
}

void Application::load_project()
{
    auto snapshot = read_project(workspace_root);
    std::vector<std::string> paths;
    for (const char* first : {"main.cpp", "camera.h", "canvas.h"})
        if (snapshot.count(first))
            paths.emplace_back(first);
    for (const auto& [path, source] : snapshot)
        if (std::find(paths.begin(), paths.end(), path) == paths.end())
            paths.push_back(path);
    for (const auto& path : paths) {
        auto panel = make_panel(path, snapshot.at(path));
        update_mesh(panel);
        if (panel.cpp && documents.size() < 3) {
            documents.push_back({panel.uri, panel.source});
            opened_documents.insert(panel.uri);
        }
        file_sizes.push_back(panel.bounds.size);
        panels.push_back(std::move(panel));
    }
    project_watcher = std::make_unique<ProjectWatcher>(workspace_root, std::move(snapshot));
}

void Application::replace_source(int index, const std::string& source)
{
    auto& old = panels[index];
    auto updated = make_panel(old.path, source);
    for (auto& fold : updated.folds) {
        const auto key = fold_identity(updated, fold.first);
        const auto previous =
            std::find_if(old.folds.begin(), old.folds.end(),
                         [&](const auto& other) { return fold_identity(old, other.first) == key; });
        if (previous != old.folds.end())
            fold.collapsed = previous->collapsed;
    }
    updated.bounds.origin = old.bounds.origin;
    updated.bounds.size.x += 32;
    updated.mesh = old.mesh;
    updated.header_mesh = old.header_mesh;
    updated.version = old.version;
    auto* server = server_for(old);
    if (server && opened_documents.count(old.uri)) {
        ++updated.version;
        server->change({updated.uri, source});
    }
    release_panel(old);
    old = std::move(updated);
    update_mesh(old);
}

void Application::apply_project_change(const ProjectChange& change)
{
    auto found = std::find_if(panels.begin(), panels.end(),
                              [&](const auto& panel) { return panel.path == change.path; });
    if (!change.source) {
        if (found == panels.end())
            return;
        activity_deleted[change.path] = {found->bounds, Clock::now()};
        auto* server = server_for(*found);
        if (server && opened_documents.erase(found->uri))
            server->close(found->uri);
        release_panel(*found);
        renderer->release(found->mesh);
        renderer->release(found->header_mesh);
        panels.erase(found);
    } else if (found == panels.end()) {
        auto panel = make_panel(change.path, *change.source);
        update_mesh(panel);
        panel.bounds.origin = {content_size.x + 96, 0};
        panels.push_back(std::move(panel));
    } else if (found->source != *change.source) {
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
    if (!shortcuts_visible)
        hide_popup();
    const auto dragged = dragged_panel >= 0 && dragged_panel < static_cast<int>(panels.size())
                             ? panels[dragged_panel].path
                             : "";
    for (const auto& change : changes)
        apply_project_change(change);
    auto index_for = [&](const std::string& path) {
        const auto found = std::find_if(panels.begin(), panels.end(),
                                        [&](const auto& panel) { return panel.path == path; });
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
    const bool runtime_change = std::any_of(changes.begin(), changes.end(), [](const auto& change) {
        return needs_viewer_build(change.path);
    });
    if (runtime_change)
        queue_rebuild();
    schedule_save();
    redraw = true;
}

void Application::refresh_project_geometry()
{
    std::vector<GraphFile> files;
    file_sizes.clear();
    for (std::size_t i = 0; i < panels.size(); ++i) {
        settle_panel(panels, i);
        const auto& panel = panels[i];
        files.push_back({panel.path, panel.source, panel.bounds.size});
        file_sizes.push_back(panel.bounds.size);
    }
    dependencies = file_dependencies(files);
    graph.routes.clear();
    graph.files.clear();
    for (const auto& panel : panels)
        graph.files.push_back(panel.bounds);
    auto bounds = graph.files;
    if (chat && chat->visible)
        bounds.push_back(chat->bounds);
    for (const auto& card : chat_windows)
        if (card->visible)
            bounds.push_back(card->bounds);
    if (intent && intent->placed && intent->visible)
        bounds.push_back(intent->bounds);
    content_size = canvas_size(bounds);
    std::ofstream(workspace_build + "/source-graph.dot") << graph_dot(files, dependencies);
    connections_dirty = true;
}

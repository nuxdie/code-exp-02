#include "application.h"

#include <type_traits>

namespace {
// The window can close while a worker is busy. Tasks own their inputs and never touch the UI.
template <class Work> auto load_in_background(Work work)
{
    std::packaged_task<std::invoke_result_t<Work>()> task(std::move(work));
    auto result = task.get_future();
    std::thread(std::move(task)).detach();
    return result;
}

template <class Result> bool ready(std::future<Result>& future)
{
    return future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

int load_priority(const std::string& path)
{
    if (path == "main.cpp")
        return 0;
    if (path == "camera.h")
        return 1;
    if (path == "canvas.h")
        return 2;
    return project_image(path) ? 4 : 3;
}
} // namespace

void Application::begin_workspace_load()
{
    const auto* root = std::getenv("SOURCE_CANVAS_PROJECT_ROOT");
    workspace_root = std::filesystem::canonical(!launch_directory.empty() ? launch_directory.c_str()
                                                : root && *root           ? root
                                                                          : project_root)
                         .string();
    if (!std::filesystem::is_directory(workspace_root))
        throw std::runtime_error("Workspace must be a directory: " + workspace_root);
    workspace_build = project_build;
    if (workspace_root != std::filesystem::weakly_canonical(project_root).string()) {
        const auto directory = std::filesystem::path(workspace_root);
        workspace_build = (directory / ".cache/source-canvas").string();
        for (const auto& candidate : {directory / "build", directory})
            if (std::filesystem::is_regular_file(candidate / "compile_commands.json"))
                workspace_build = candidate.string();
        std::filesystem::create_directories(workspace_build);
    }
    workspace_load->reading = load_in_background([root = workspace_root] {
        auto files = list_project_files(root);
        std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) {
            const int first = load_priority(a.path), second = load_priority(b.path);
            return first == second ? a.path < b.path : first < second;
        });
        return files;
    });
}

void Application::advance_workspace_load()
{
    auto& load = *workspace_load;
    if (load.reading.valid()) {
        if (!ready(load.reading))
            return;
        prepare_workspace_preview(load.reading.get());
        return;
    }
    if (load.next != load.preview_next && Clock::now() >= load.preview_due)
        redraw = true;
    const auto deadline = Clock::now() + std::chrono::milliseconds(8);
    while (load.next < load.files.size()) {
        if (!add_workspace_panel() || load.next == 1 || Clock::now() >= deadline)
            return;
    }
    if (!load.layout.valid()) {
        const bool complete = panels.size() == load.files.size();
        std::vector<GraphFile> files;
        files.reserve(panels.size());
        for (const auto& panel : panels)
            files.push_back(
                {panel.path, complete ? std::string{} : panel.source, panel.bounds.size});
        load.layout = load_in_background(
            [files = std::move(files), snapshot = std::move(load.snapshot), edges = load.edges,
             complete, root = workspace_root, build = workspace_build]() mutable {
                WorkspaceLayout result;
                result.dependencies = complete ? std::move(edges) : file_dependencies(files);
                result.graph = folder_layout(files, result.dependencies);
                if (!files.empty())
                    std::ofstream(build + "/source-graph.dot")
                        << graph_dot(files, result.dependencies);
                result.watcher = std::make_unique<ProjectWatcher>(root, std::move(snapshot));
                return result;
            });
        return;
    }
    if (ready(load.layout)) {
        auto result = load.layout.get();
        dependencies = std::move(result.dependencies);
        graph = std::move(result.graph);
        project_watcher = std::move(result.watcher);
        apply_graph_layout();
        finish_workspace_load();
    }
}

void Application::prepare_workspace_preview(std::vector<ProjectFile> files)
{
    auto& load = *workspace_load;
    panels.reserve(files.size());
    load.files.reserve(files.size());
    load.titles.resize(files.size());
    for (auto& file : files) {
        load.index.emplace(file.path, static_cast<int>(load.files.size()));
        const auto lines = std::clamp(file.bytes / 42, std::uintmax_t{3}, std::uintmax_t{160});
        const Vec2 size = project_image(file.path)
                              ? Vec2{400, 280}
                              : Vec2{620, header_height + static_cast<double>(lines) * line_height};
        load.files.push_back({std::move(file.path), {}, size});
    }
    load.preview = folder_layout(load.files, {});
    start_workspace_file();
    redraw = true;
}

void Application::start_workspace_file()
{
    auto& load = *workspace_load;
    if (load.next == load.files.size())
        return;
    load.detail = load_in_background([root = workspace_root, path = load.files[load.next].path] {
        WorkspaceFile file;
        file.source = read_project_file(root, path);
        if (file.source && project_image(path))
            file.image = decode_image(*file.source);
        return file;
    });
}

bool Application::add_workspace_panel()
{
    auto& load = *workspace_load;
    if (!ready(load.detail))
        return false;
    auto detail = load.detail.get();
    const auto index = load.next++;
    start_workspace_file();
    auto& file = load.files[index];
    if (!detail.source) {
        file.size = {};
        return true;
    }

    auto panel = make_panel(file.path, *detail.source, detail.image ? &*detail.image : nullptr);
    update_mesh(panel);
    if (panel.cpp && documents.size() < 3) {
        documents.push_back({panel.uri, panel.source});
        opened_documents.insert(panel.uri);
    }
    file.size = panel.bounds.size;
    const auto edges =
        scan_dependencies(load.files, load.index, static_cast<int>(index), panel.source);
    load.edges.insert(load.edges.end(), edges.begin(), edges.end());
    load.snapshot.emplace(file.path, std::move(*detail.source));
    file_sizes.push_back(panel.bounds.size);
    load.panel_indices.push_back(index);
    panels.push_back(std::move(panel));
    redraw |= panels.size() == 1;
    return true;
}

void Application::draw_workspace_preview(const Viewport& view)
{
    auto& load = *workspace_load;
    const auto saved_camera = camera;
    if (load.next != load.preview_next && Clock::now() >= load.preview_due) {
        load.preview = folder_layout(load.files, load.edges);
        std::vector<GpuPath> paths;
        for (const auto& edge : load.edges) {
            const auto& from = load.preview.files[edge.source];
            const auto& to = load.preview.files[edge.target];
            if (load.files[edge.target].size.x > 0)
                paths.push_back(
                    {{{from.origin.x + from.size.x, from.origin.y + header_height / 2.0},
                      {to.origin.x, to.origin.y + header_height / 2.0}},
                     {0.55f, 0.63f, 0.73f, 0.45f},
                     1,
                     false});
        }
        renderer->paths(load.connections, paths);
        load.preview_next = load.next;
        load.preview_due = Clock::now() + std::chrono::milliseconds(80);
    }
    auto preview_view = view;
    preview_view.height = std::max(1.0, view.height - 44);
    camera.fit(load.preview.size, preview_view);
    camera.center.y += 22 / camera.zoom;
    for (std::size_t i = 0; i < panels.size(); ++i)
        panels[i].bounds.origin = load.preview.files[load.panel_indices[i]].origin;

    renderer->begin(width, height);
    if (show_connections)
        renderer->draw(load.connections, camera.world_to_screen({}, view), camera.zoom);
    const CanvasRect screen{{0, 0}, {view.width, preview_view.height}};
    const bool show_body = camera.zoom >= 0.18;
    int title_budget = 16;
    for (std::size_t i = show_body ? load.next : 0; i < load.preview.files.size(); ++i) {
        const auto& card = load.preview.files[i];
        const CanvasRect bounds{camera.world_to_screen(card.origin, view),
                                {card.size.x * camera.zoom, card.size.y * camera.zoom}};
        if (bounds.size.x <= 0 || !bounds.intersects(screen))
            continue;
        renderer->rectangle(bounds, {0.91f, 0.93f, 0.96f, 0.62f});
        if (i < load.next || !show_body)
            continue;
        auto& title = load.titles[i];
        if (!title.buffer && title_budget > 0) {
            auto* layout = make_line(load.files[i].path, {});
            renderer->upload(title, {{layout, {0, 0}}});
            g_object_unref(layout);
            --title_budget;
        }
        renderer->clip(bounds, height);
        renderer->draw(title, {bounds.origin.x + 4, bounds.origin.y + 2},
                       std::max(0.5, camera.zoom));
        renderer->clear_clip();
    }
    if (show_body)
        draw_files(view);
    draw_headers(view);
    renderer->rectangle({{0, view.height - 44}, {view.width, 44}}, {1, 1, 1, 1});
    renderer->draw(load.label, {16, view.height - 34}, 1);
    renderer->present();
    camera = saved_camera;
}

void Application::finish_workspace_load()
{
    if (!benchmark) {
        try {
            language_server = std::make_unique<lsp::LanguageServer>(
                clangd_executable, workspace_root, workspace_build, std::move(documents));
        } catch (const std::exception& error) {
            language_failure = error.what();
        }
    }
    initialize_cmake();
    camera.reset(viewport());
    initialize_chat();
    load_intent_margin();
    initialize_reload();
    for (auto& title : workspace_load->titles)
        renderer->release(title);
    renderer->release(workspace_load->label);
    renderer->release(workspace_load->connections);
    workspace_load.reset();
    redraw = true;
}

void Application::open_directory_picker()
{
    if (directory_picker)
        return;
    try {
        directory_picker = std::make_unique<DirectoryPicker>(workspace_root, window);
    } catch (const std::exception& error) {
        show_popup(error.what(), viewport().center(), true);
    }
}

void Application::poll_directory_picker()
{
    if (!directory_picker)
        return;
    const auto selection = directory_picker->take_result();
    if (!selection)
        return;
    directory_picker.reset();
    if (!selection->error.empty()) {
        show_popup(selection->error, viewport().center(), true);
        return;
    }
    if (selection->directory.empty())
        return;
    std::error_code error;
    const auto directory = std::filesystem::canonical(selection->directory, error);
    if (error || !std::filesystem::is_directory(directory, error)) {
        show_popup("Could not open directory: " + selection->directory, viewport().center(), true);
        return;
    }
    if (directory.string() == workspace_root)
        return;
    // Keep the old workspace intact until its session and services have shut down.
    save_session();
    launch_directory = directory.string();
    reload_requested = true;
    running = false;
}

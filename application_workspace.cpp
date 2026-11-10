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

std::vector<std::string> workspace_paths(const ProjectSnapshot& snapshot)
{
    std::vector<std::string> paths, images;
    const std::array<std::string_view, 3> first{"main.cpp", "camera.h", "canvas.h"};
    for (const auto path : first)
        if (snapshot.count(std::string(path)))
            paths.emplace_back(path);
    for (const auto& [path, source] : snapshot) {
        if (project_image(path))
            images.push_back(path);
        else if (std::find(first.begin(), first.end(), path) == first.end())
            paths.push_back(path);
    }
    // Source cards can appear before the image decoder starts.
    paths.insert(paths.end(), std::make_move_iterator(images.begin()),
                 std::make_move_iterator(images.end()));
    return paths;
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
    workspace_load->reading =
        load_in_background([root = workspace_root] { return read_project(root); });
}

void Application::advance_workspace_load()
{
    auto& load = *workspace_load;
    if (load.reading.valid()) {
        if (!ready(load.reading))
            return;
        load.snapshot = load.reading.get();
        load.paths = workspace_paths(load.snapshot);
        panels.reserve(load.paths.size());
    }
    const auto deadline = Clock::now() + std::chrono::milliseconds(8);
    while (load.next < load.paths.size()) {
        if (!add_workspace_panel() || load.next == 1 || Clock::now() >= deadline)
            return;
    }
    if (!load.layout.valid()) {
        std::vector<GraphFile> files;
        files.reserve(panels.size());
        for (const auto& panel : panels)
            files.push_back({panel.path, panel.source, panel.bounds.size});
        load.layout =
            load_in_background([files = std::move(files), snapshot = std::move(load.snapshot),
                                root = workspace_root, build = workspace_build]() mutable {
                WorkspaceLayout result;
                result.dependencies = file_dependencies(files);
                result.graph = layout_files(files, result.dependencies, build);
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

bool Application::add_workspace_panel()
{
    auto& load = *workspace_load;
    const auto& path = load.paths[load.next];
    const auto& source = load.snapshot.at(path);
    std::optional<ImagePreview> preview;
    if (project_image(path)) {
        if (!load.image.valid())
            load.image = load_in_background([source] { return decode_image(source); });
        if (!ready(load.image))
            return false;
        preview = load.image.get();
    }
    auto panel = make_panel(path, source, preview ? &*preview : nullptr);
    update_mesh(panel);
    if (panel.cpp && documents.size() < 3) {
        documents.push_back({panel.uri, panel.source});
        opened_documents.insert(panel.uri);
    }
    file_sizes.push_back(panel.bounds.size);
    panels.push_back(std::move(panel));
    ++load.next;
    redraw = true;
    return true;
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
    initialize_intent();
    initialize_reload();
    renderer->release(workspace_load->label);
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

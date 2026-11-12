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
    // Positions saved under an older arrangement are not worth restoring.
    auto session = read_session();
    if (session.is_object() && session.value("layout", "") == "folders")
        workspace_load->session = std::move(session);
    workspace_load->reading = load_in_background([root = workspace_root] {
        auto files = list_project_files(root);
        for (auto& file : files) {
            const auto text =
                project_image(file.path) ? std::nullopt : read_project_file(root, file.path);
            if (!text)
                continue;
            file.rows = folded_rows(*text, file.path);
            file.columns = text_extent(*text).second;
        }
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
        load.layout =
            load_in_background([files = std::move(files), snapshot = std::move(load.snapshot),
                                edges = load.edges, complete, root = workspace_root]() mutable {
                WorkspaceLayout result;
                result.dependencies = complete ? std::move(edges) : file_dependencies(files);
                result.watcher = std::make_unique<ProjectWatcher>(root, std::move(snapshot));
                return result;
            });
        return;
    }
    if (ready(load.layout)) {
        auto result = load.layout.get();
        dependencies = std::move(result.dependencies);
        project_watcher = std::move(result.watcher);
        finish_plan();
        finish_workspace_load();
    }
}

// How big a file's card will be, from its line and column counts alone.
Vec2 Application::estimated_size(const ProjectFile& file, double cell) const
{
    const double title = static_cast<double>(file.path.size()) * cell + 64;
    // An image is shown at most 640 wide; its height is known only once it is decoded.
    if (project_image(file.path))
        return {std::max(title, 640.0), header_height + 360.0};
    return {std::max(title, file.columns * cell) + 36,
            header_height + static_cast<double>(file.rows) * line_height};
}

// A file whose real size differs from the plan moves only what was planned beneath it.
void Application::settle_plan(std::size_t index, Vec2 size)
{
    auto& load = *workspace_load;
    auto& own = load.plan[index];
    const double delta = size.y - own.size.y, bottom = own.origin.y + own.size.y;
    const double left = own.origin.x, right = left + std::max(own.size.x, size.x);
    own.size = size;
    if (delta == 0)
        return;
    for (auto& other : load.plan)
        if (&other != &own && other.origin.y >= bottom && other.origin.x < right &&
            other.origin.x + other.size.x > left)
            other.origin.y += delta;
    for (std::size_t i = 0; i < panels.size(); ++i)
        panels[i].bounds.origin = load.plan[load.panel_indices[i]].origin;
}

// The plan becomes the canvas. Only if real sizes made files collide is it arranged again.
void Application::finish_plan()
{
    graph.files.clear();
    for (const auto& panel : panels)
        graph.files.push_back(panel.bounds);
    graph.size = canvas_size(graph.files);
    apply_graph_layout();
    for (std::size_t i = 0; i < panels.size(); ++i)
        for (std::size_t j = i + 1; j < panels.size(); ++j)
            if (panels[i].bounds.intersects(panels[j].bounds)) {
                relayout();
                return;
            }
}

// Every file gets its place before any is read: where it was last time, or for a project
// seen for the first time, where its line and column counts say it will fit.
void Application::prepare_workspace_preview(std::vector<ProjectFile> files)
{
    auto& load = *workspace_load;
    const auto saved = load.session.is_object() ? load.session.value("files", lsp::Json::object())
                                                : lsp::Json::object();
    // Files the saved canvas does not know come last, so they can join their folder.
    std::stable_partition(files.begin(), files.end(), [&](const auto& file) {
        return !load.session.is_object() || saved.contains(file.path);
    });
    auto* sample = pango_layout_copy(base_layout);
    pango_layout_set_text(sample, "0000000000", -1);
    int ten = 0;
    pango_layout_get_pixel_size(sample, &ten, nullptr);
    g_object_unref(sample);
    panels.reserve(files.size());
    load.files.reserve(files.size());
    for (auto& file : files) {
        load.index.emplace(file.path, static_cast<int>(load.files.size()));
        const auto place = saved.value(file.path, lsp::Json::object());
        const auto size = place.contains("w") ? Vec2{place.at("w"), place.at("h")}
                                              : estimated_size(file, ten / 10.0);
        load.plan.push_back(place.contains("x") ? CanvasRect{{place.at("x"), place.at("y")}, size}
                                                : CanvasRect{});
        load.files.push_back({std::move(file.path), {}, size});
    }
    if (load.session.is_object()) {
        restore_window(load.session);
        const auto view = load.session.value("camera", lsp::Json::object());
        if (view.contains("zoom")) {
            camera.center = {view.at("x"), view.at("y")};
            camera.zoom = std::clamp(view.value("zoom", 1.0), Camera::min_zoom, Camera::max_zoom);
        }
    } else {
        load.plan = folder_layout(load.files, {}).files;
        camera.fit(canvas_size(load.plan), viewport());
        camera.zoom = std::min(camera.zoom, 1.0);
    }
    zoom_animation.cancel(camera);
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
        settle_plan(index, {});
        return true;
    }

    auto panel = make_panel(file.path, *detail.source, detail.image ? &*detail.image : nullptr);
    if (load.session.is_object())
        restore_folds(panel, load.session["files"].value(file.path, lsp::Json::object()));
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
    const bool planned = load.plan[index].size.x > 0;
    panel.bounds.origin = load.plan[index].origin;
    panels.push_back(std::move(panel));
    if (planned) {
        settle_plan(index, panels.back().bounds.size);
    } else {
        place_in_folder(panels.back());
        load.plan[index] = panels.back().bounds;
    }
    if (Clock::now() >= load.draw_due) {
        load.draw_due = Clock::now() + std::chrono::milliseconds(50);
        redraw = true;
    }
    return true;
}

// While files arrive the canvas is already the canvas: a file not read yet is a faint
// card where it will be.
void Application::draw_workspace_preview(const Viewport& view)
{
    auto& load = *workspace_load;
    renderer->begin(width, height);
    draw_regions(view);
    const CanvasRect screen{{0, 0}, {view.width, view.height}};
    for (std::size_t i = load.next; i < load.plan.size(); ++i) {
        const auto& card = load.plan[i];
        const CanvasRect bounds{camera.world_to_screen(card.origin, view),
                                {card.size.x * camera.zoom, card.size.y * camera.zoom}};
        if (bounds.size.x > 0 && bounds.intersects(screen))
            renderer->rectangle(bounds, {0.91f, 0.93f, 0.96f, 0.62f});
    }
    draw_files(view);
    draw_headers(view);
    renderer->present();
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
    initialize_chat();
    load_intent_margin();
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

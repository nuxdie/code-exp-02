#include "application.h"

void Application::initialize_workspace()
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
    load_project();
    relayout();
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

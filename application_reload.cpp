#include "application.h"

#include <unistd.h>

void Application::initialize_reload()
{
    const auto* option = std::getenv("SOURCE_CANVAS_AUTO_BUILD");
    auto_build = workspace_root == std::filesystem::weakly_canonical(project_root).string() &&
                 !benchmark && chat && !chat->disabled && (!option || std::string(option) != "0");
    std::error_code error;
    loaded_binary = std::filesystem::last_write_time(executable, error);
    observed_binary = loaded_binary;
    if (auto_build) {
        live_build = std::make_unique<LiveBuild>(project_build);
        binary_check_due = Clock::now() + std::chrono::seconds(1);
    }
}
void Application::queue_rebuild()
{
    if (!auto_build)
        return;
    ++source_generation;
    rebuild_due = Clock::now() + std::chrono::seconds(2);
}
void Application::poll_reload(Clock::time_point now)
{
    if (!auto_build)
        return;
    if (deferred_reload && !agents_pending())
        request_reload();
    for (const auto& result : live_build->take_results())
        handle_build_result(result);
    if (rebuild_due <= now && !build_running) {
        live_build->request(source_generation);
        build_running = true;
        rebuild_due = Clock::time_point::max();
        build_status = "Building viewer…";
        redraw = true;
    }
    if (binary_check_due > now)
        return;
    binary_check_due = now + std::chrono::seconds(1);
    std::error_code error;
    const auto current = std::filesystem::last_write_time(executable, error);
    if (error || current == loaded_binary || build_running)
        return;
    if (current != observed_binary) {
        observed_binary = current;
        return;
    }
    if (access(executable.c_str(), X_OK) == 0)
        request_reload();
}
void Application::handle_build_result(const BuildResult& result)
{
    if (result.started) {
        build_running = true;
        build_status = "Building viewer…";
        return;
    }
    build_running = false;
    build_status = result.success ? "Viewer up to date" : "Build failed · source is live";
    if (chat) {
        const std::string id = "canvas-build-" + std::to_string(result.generation);
        auto& entries = chat->conversation.entries;
        const auto existing = std::find_if(entries.begin(), entries.end(),
                                           [&](const auto& entry) { return entry.id == id; });
        if (existing == entries.end()) {
            ConversationEntry entry;
            entry.id = id;
            entry.turn = chat->conversation.active_turn;
            entry.role = "Build";
            entry.text = build_status;
            entry.detail = result.output;
            entry.activity = true;
            entries.push_back(std::move(entry));
        }
        chat->conversation.error =
            result.success ? "" : "Build failed; the running viewer is still available.";
    }
    redraw = true;
    if (result.generation != source_generation) {
        rebuild_due = Clock::now() + std::chrono::milliseconds(250);
        return;
    }
    if (result.success)
        request_reload();
}
void Application::request_reload()
{
    if (agents_pending()) {
        deferred_reload = true;
        build_status = "Viewer update ready";
        return;
    }
    deferred_reload = false;
    save_session();
    reload_requested = true;
    running = false;
}

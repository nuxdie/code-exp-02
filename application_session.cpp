#include "application.h"
#include <X11/Xutil.h>

namespace {
std::filesystem::path session_path(const std::string& root)
{
    if (const auto* path = std::getenv("SOURCE_CANVAS_STATE_FILE"))
        return path;
    return std::filesystem::path(root) / ".cache/canvas-session.json";
}
} // namespace

void Application::load_session()
{
    if (!chat || benchmark || std::getenv("SOURCE_CANVAS_DISABLE_SESSION"))
        return;
    const char* environment_thread = std::getenv("CODEX_THREAD_ID");
    if (environment_thread)
        chat->conversation.thread_id = environment_thread;
    std::ifstream input(session_path(workspace_root));
    if (!input)
        return;
    const auto state = lsp::Json::parse(input, nullptr, false);
    if (!state.is_object() || state.value("root", "") != workspace_root)
        return;
    try {
        intent_session = state.value("intent", lsp::Json::object());
        const auto origin = state.value("originThread", "");
        load_agent_window(*chat, state);
        if (environment_thread && origin != environment_thread) {
            chat->conversation.thread_id = environment_thread;
            chat->conversation.fresh = false;
            chat->context = lsp::Json::object();
        }
        chat_windows.clear();
        for (const auto& saved : state.value("additionalAgents", lsp::Json::array())) {
            auto card = std::make_unique<ChatCanvas>(workspace_root);
            load_agent_window(*card, saved);
            chat_windows.push_back(std::move(card));
        }
        for (auto& panel : panels) {
            const auto saved =
                state.value("files", lsp::Json::object()).value(panel.path, lsp::Json::object());
            if (!saved.contains("x"))
                continue;
            panel.bounds.origin = {saved.at("x"), saved.at("y")};
            for (auto& fold : panel.folds) {
                const auto folds = saved.value("folds", lsp::Json::object());
                fold.collapsed =
                    folds.value(fold_identity(panel, fold.first),
                                folds.value(std::to_string(fold.first), fold.collapsed));
            }
            update_mesh(panel);
        }
        const auto view = state.value("camera", lsp::Json::object());
        if (view.contains("zoom")) {
            camera.center = {view.at("x"), view.at("y")};
            camera.zoom = std::clamp(view.value("zoom", 1.0), Camera::min_zoom, Camera::max_zoom);
            zoom_animation.cancel(camera);
            initial_view = false;
        }
        const auto window = state.value("window", lsp::Json::object());
        if (window.contains("width")) {
            width = std::max(320, window.value("width", width));
            height = std::max(240, window.value("height", height));
            XSizeHints hints{};
            hints.flags = PPosition | PSize | PWinGravity;
            hints.win_gravity = StaticGravity;
            hints.width = width;
            hints.height = height;
            hints.x = window.value("x", 0);
            hints.y = window.value("y", 0);
            XSetWMNormalHints(display, this->window, &hints);
            XMoveResizeWindow(display, this->window, window.value("x", 0), window.value("y", 0),
                              width, height);
        }
    } catch (const lsp::Json::exception&) {
        // A partially written or older view-state file must never prevent opening the project.
    }
}
void Application::save_session()
{
    if (!chat || chat->disabled || benchmark || window_destroyed ||
        std::getenv("SOURCE_CANVAS_DISABLE_SESSION"))
        return;
    lsp::Json files = lsp::Json::object();
    for (const auto& panel : panels) {
        lsp::Json folds = lsp::Json::object();
        for (const auto& fold : panel.folds)
            folds[fold_identity(panel, fold.first)] = fold.collapsed;
        files[panel.path] = {
            {"x", panel.bounds.origin.x}, {"y", panel.bounds.origin.y}, {"folds", folds}};
    }
    int x = 0, y = 0;
    Window child;
    XTranslateCoordinates(display, window, RootWindow(display, screen), 0, 0, &x, &y, &child);
    const auto* origin = std::getenv("CODEX_THREAD_ID");
    auto state = save_agent_window();
    lsp::Json additional = lsp::Json::array();
    for (auto& card : chat_windows) {
        std::swap(chat, card);
        additional.push_back(save_agent_window());
        std::swap(chat, card);
    }
    state.update(
        {{"root", workspace_root},
         {"originThread", origin ? origin : ""},
         {"additionalAgents", additional},
         {"camera", {{"x", camera.center.x}, {"y", camera.center.y}, {"zoom", camera.zoom}}},
         {"window", {{"x", x}, {"y", y}, {"width", width}, {"height", height}}},
         {"files", files},
         {"intent", intent ? lsp::Json{{"x", intent->bounds.origin.x},
                                       {"y", intent->bounds.origin.y},
                                       {"selected", intent->selected},
                                       {"overview", intent->overview},
                                       {"scroll", intent->scroll}}
                           : lsp::Json::object()}});
    const auto path = session_path(workspace_root);
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    const auto temporary = path.string() + ".tmp";
    std::ofstream(temporary) << state.dump(2);
    std::filesystem::rename(temporary, path, error);
    save_due = Clock::time_point::max();
}
void Application::schedule_save()
{
    save_due = Clock::now() + std::chrono::milliseconds(500);
}

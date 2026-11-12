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

void Application::load_agent_windows(const lsp::Json& state, const char* environment_thread)
{
    const auto origin = state.value("originThread", "");
    load_agent_window(*chat, state);
    if (environment_thread && origin != environment_thread) {
        chat->conversation.thread_id = environment_thread;
        chat->conversation.fresh = false;
        chat->composing = false;
        chat->bounds.size = {760, 900};
        chat->context = lsp::Json::object();
    }
    chat_windows.clear();
    for (const auto& saved : state.value("additionalAgents", lsp::Json::array())) {
        auto card = std::make_unique<ChatCanvas>(workspace_root);
        load_agent_window(*card, saved);
        // A closed conversation with nothing in it is not worth keeping.
        if (card->visible || kept_chat(*card))
            chat_windows.push_back(std::move(card));
    }
}

// The saved canvas for this project, or null when there is none to use.
lsp::Json Application::read_session() const
{
    if (benchmark || std::getenv("SOURCE_CANVAS_DISABLE_SESSION"))
        return nullptr;
    std::ifstream input(session_path(workspace_root));
    if (!input)
        return nullptr;
    auto state = lsp::Json::parse(input, nullptr, false);
    if (!state.is_object() || state.value("root", "") != workspace_root)
        return nullptr;
    return state;
}

void Application::restore_window(const lsp::Json& state)
{
    const auto window = state.value("window", lsp::Json::object());
    if (!window.contains("width"))
        return;
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
    XMoveResizeWindow(display, this->window, window.value("x", 0), window.value("y", 0), width,
                      height);
}

void Application::load_session()
{
    if (!chat || benchmark || std::getenv("SOURCE_CANVAS_DISABLE_SESSION"))
        return;
    const char* environment_thread = std::getenv("CODEX_THREAD_ID");
    if (environment_thread)
        chat->conversation.thread_id = environment_thread;
    const auto state = read_session();
    if (!state.is_object())
        return;
    try {
        appearance = state.value("appearance", "system");
        if (appearance != "light" && appearance != "dark")
            appearance = "system";
        renderer->set_dark_theme(appearance == "system" ? desktop_dark : appearance == "dark");
        set_monospace_font(state.contains("monospaceFont") &&
                                   state["monospaceFont"] == "Victor Mono"
                               ? MonospaceFont::Victor
                               : MonospaceFont::JetBrains);
        const auto saved_walkthrough = state.value("walkthrough", lsp::Json());
        if (saved_walkthrough.is_object()) {
            try {
                show_walkthrough(saved_walkthrough, true);
            } catch (const std::exception&) {
                // Invalid walkthroughs must not block restoration of the rest of the session.
            }
        }
        const auto saved_search = state.value("globalSearch", lsp::Json::object());
        search.query.set(saved_search.value("query", ""));
        search.active = saved_search.value("active", false);
        if (search.active)
            update_search();
        load_agent_windows(state, environment_thread);
        // Positions saved under an older arrangement would scatter the folders again.
        const bool placed = state.value("layout", "") == "folders";
        restore_file_layouts(state.value("files", lsp::Json::object()), placed);
        if (!placed || !state.value("shelved", false))
            shelve_conversations();
        const auto view = state.value("camera", lsp::Json::object());
        if (placed && view.contains("zoom")) {
            camera.center = {view.at("x"), view.at("y")};
            camera.zoom = std::clamp(view.value("zoom", 1.0), Camera::min_zoom, Camera::max_zoom);
            zoom_animation.cancel(camera);
            initial_view = false;
        }
        load_navigation(state.value("cameraHistory", lsp::Json::object()));
        restore_window(state);
    } catch (const lsp::Json::exception&) {
        // A partially written or older view-state file must never prevent opening the project.
    }
}
bool Application::restore_folds(SourcePanel& panel, const lsp::Json& saved)
{
    const auto folds = saved.value("folds", lsp::Json::object());
    bool changed = false;
    for (auto& fold : panel.folds) {
        const auto collapsed = folds.value(fold_identity(panel, fold.first),
                                           folds.value(std::to_string(fold.first), fold.collapsed));
        changed |= collapsed != fold.collapsed;
        fold.collapsed = collapsed;
    }
    return changed;
}

void Application::restore_file_layouts(const lsp::Json& files, bool placed)
{
    // Files that appeared since the session was saved have no place yet.
    std::set<std::string> unplaced;
    for (auto& panel : panels) {
        const auto saved = files.value(panel.path, lsp::Json::object());
        if (!saved.contains("x")) {
            unplaced.insert(panel.path);
            continue;
        }
        if (placed)
            panel.bounds.origin = {saved.at("x"), saved.at("y")};
        if (restore_folds(panel, saved))
            update_mesh(panel);
    }
    if (!placed)
        return;
    for (auto& panel : panels)
        if (unplaced.erase(panel.path))
            place_in_folder(panel, unplaced);
}

void Application::save_session()
{
    save_due = Clock::time_point::max();
    if (!chat || chat->disabled || benchmark || window_destroyed ||
        std::getenv("SOURCE_CANVAS_DISABLE_SESSION"))
        return;
    lsp::Json files = lsp::Json::object();
    for (const auto& panel : panels) {
        lsp::Json folds = lsp::Json::object();
        for (const auto& fold : panel.folds)
            folds[fold_identity(panel, fold.first)] = fold.collapsed;
        files[panel.path] = {{"x", panel.bounds.origin.x},
                             {"y", panel.bounds.origin.y},
                             {"w", panel.bounds.size.x},
                             {"h", panel.bounds.size.y},
                             {"folds", folds}};
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
         {"appearance", appearance},
         {"monospaceFont", monospace_family()},
         {"originThread", origin ? origin : ""},
         {"globalSearch", {{"query", search.query.text}, {"active", search.active}}},
         {"additionalAgents", additional},
         {"walkthrough", save_walkthrough()},
         {"camera", {{"x", camera.center.x}, {"y", camera.center.y}, {"zoom", camera.zoom}}},
         {"cameraHistory", save_navigation()},
         {"window", {{"x", x}, {"y", y}, {"width", width}, {"height", height}}},
         {"layout", "folders"},
         {"shelved", true},
         {"files", files}});
    const auto path = session_path(workspace_root);
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    const auto temporary = path.string() + ".tmp";
    std::ofstream(temporary) << state.dump(2);
    std::filesystem::rename(temporary, path, error);
}
void Application::schedule_save()
{
    save_due = Clock::now() + std::chrono::milliseconds(500);
}

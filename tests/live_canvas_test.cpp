#include "application.h"

#include <epoxy/gl.h>
#include <unistd.h>

void expect(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

struct ApplicationTestAccess {
    template <class Predicate> static void wait_for(Application& app, Predicate predicate)
    {
        const auto deadline = Application::Clock::now() + std::chrono::seconds(10);
        while (!predicate() && Application::Clock::now() < deadline) {
            pollfd fds[]{{app.project_watcher->event_fd(), POLLIN, 0},
                         {app.language_server ? app.language_server->event_fd() : -1, POLLIN, 0}};
            poll(fds, 2, 50);
            app.poll_project();
            if (app.language_server)
                for (const auto& message : app.language_server->take_messages())
                    app.handle_message(message);
        }
        if (!predicate()) {
            for (const auto& panel : app.panels)
                std::cerr << panel.path << " version=" << panel.version
                          << " diagnostics=" << panel.diagnostics.dump()
                          << " symbols=" << panel.symbols.size() << "\n";
        }
        expect(predicate(), "Timed out waiting for the live canvas");
    }
    static SourcePanel& panel(Application& app, const char* path)
    {
        const auto found = std::find_if(app.panels.begin(), app.panels.end(),
                                        [&](const auto& panel) { return panel.path == path; });
        if (found == app.panels.end())
            throw std::runtime_error(std::string("Missing source panel: ") + path);
        return *found;
    }
    static void test_live_source(Application& app, const std::filesystem::path& root)
    {
        app.initial_view = false;
        app.camera.center = {123, 456};
        app.camera.zoom = 0.7;
        app.zoom_animation.cancel(app.camera);
        const auto buffer = panel(app, "main.cpp").mesh.buffer;
        const auto origin = panel(app, "main.cpp").bounds.origin;
        std::filesystem::create_directories(root / "child");
        std::ofstream(root / "child/new.h") << "#pragma once\n";
        const std::string updated =
            "#include \"child/new.h\"\n\nstruct Changed {\n int value = 42;\n};\n";
        std::ofstream(root / "edit.tmp") << updated;
        std::filesystem::rename(root / "edit.tmp", root / "main.cpp");
        wait_for(app, [&] {
            return panel(app, "main.cpp").source == updated && app.panels.size() == 3;
        });
        const auto& main = panel(app, "main.cpp");
        expect(app.camera.center.x == 123 && app.camera.center.y == 456 && app.camera.zoom == 0.7,
               "Live edits must preserve the exact camera position and zoom");
        expect(main.mesh.buffer == buffer && main.bounds.origin.x == origin.x &&
                   main.bounds.origin.y == origin.y,
               "Live edits must reuse GPU buffers and preserve the edited file's position");
        expect(app.dependencies.size() == 1 &&
                   app.panels[app.dependencies[0].target].path == "child/new.h",
               "Dependency arrows must update to newly added files");
        wait_for(app, [&] {
            const auto& current = panel(app, "main.cpp");
            return std::any_of(current.symbols.begin(), current.symbols.end(),
                               [](const auto& symbol) { return symbol.name == "Changed"; });
        });
        std::ofstream(root / "main.cpp") << "int bad = missing_after_reload;\n";
        wait_for(app, [&] {
            const auto& current = panel(app, "main.cpp");
            return current.source.find("missing_after_reload") != std::string::npos &&
                   std::any_of(
                       current.diagnostics.begin(), current.diagnostics.end(),
                       [](const auto& diagnostic) {
                           return diagnostic.value("message", "").find("missing_after_reload") !=
                                  std::string::npos;
                       });
        });
        std::ofstream(root / "main.cpp") << updated;
        wait_for(app, [&] {
            const auto& current = panel(app, "main.cpp");
            return current.source == updated && !current.symbols.empty() &&
                   std::none_of(
                       current.diagnostics.begin(), current.diagnostics.end(),
                       [](const auto& diagnostic) {
                           return diagnostic.value("severity", 1) == 1 ||
                                  diagnostic.value("message", "").find("missing_after_reload") !=
                                      std::string::npos;
                       });
        });
        const auto uri = panel(app, "main.cpp").uri;
        app.handle_message({"textDocument/documentSymbol", uri, 99, lsp::Json::array(), {}, 1});
        expect(!panel(app, "main.cpp").symbols.empty(),
               "Stale language responses must not replace current symbols");
        std::filesystem::remove(root / "widget.h");
        wait_for(app, [&] { return app.panels.size() == 2; });
        app.draw();
        expect(glGetError() == GL_NO_ERROR,
               "Reloaded source and arrows must render without GPU errors");
    }
    static void test_composer_and_state(Application& app, const std::filesystem::path& root)
    {
        app.chat->conversation.ready = true;
        app.chat->conversation.thread_id = "fixture-thread";
        app.chat->conversation.active_turn.clear();
        app.focus_chat();
        app.chat->input.set("Keep this draft 🌍");
        app.draw();
        XEvent key{};
        key.type = KeyPress;
        key.xkey.display = app.display;
        key.xkey.window = app.window;
        key.xkey.keycode = XKeysymToKeycode(app.display, XK_Return);
        app.handle_key(key);
        const auto outgoing = app.chat->conversation.outgoing();
        expect(
            outgoing.size() == 1 && outgoing[0]["method"] == "turn/start" &&
                app.chat->input.text == "Keep this draft 🌍",
            "Enter must submit from the native composer while retaining an unacknowledged draft");
        app.chat->conversation.receive(
            {{"id", outgoing[0]["id"]}, {"result", {{"turn", {{"id", "fixture-turn"}}}}}});
        app.chat->disabled = false;
        app.poll_agent();
        expect(app.chat->input.text.empty(), "Only an accepted message may clear the composer");
        app.chat->input.set("An unsent multiline\ndraft with λ");
        app.chat->input.place(5);
        app.chat->scroll = 73;
        app.chat->follow = false;
        const auto camera = app.camera;
        app.save_session();
        app.chat->input.set("");
        app.camera.center = {};
        app.load_session();
        expect(app.chat->input.text == "An unsent multiline\ndraft with λ" &&
                   app.chat->input.cursor == 5 && app.chat->scroll == 73 &&
                   app.camera.center.x == camera.center.x && app.camera.zoom == camera.zoom,
               "Reload state must preserve the draft, caret, scroll position and camera");
        expect(std::filesystem::exists(root / ".cache/canvas-session.json"),
               "The view state must be saved inside the project cache");
        app.chat->conversation.questions.push_back(
            {1, "item/commandExecution/requestApproval", "approval", lsp::Json::object()});
        app.request_reload();
        expect(app.deferred_reload && !app.reload_requested,
               "A viewer reload must wait for an actual pending approval to be answered");
        app.chat->conversation.questions.clear();
        app.request_reload();
        expect(app.reload_requested && !app.running,
               "A ready viewer must request a state-preserving restart");
    }
    static void run(Application& app, const std::filesystem::path& root)
    {
        expect(app.initialize(), "Live canvas must initialize");
        const auto deadline = Application::Clock::now() + std::chrono::seconds(2);
        XWindowAttributes attributes{};
        do {
            XGetWindowAttributes(app.display, app.window, &attributes);
            if (attributes.map_state == IsViewable)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        } while (Application::Clock::now() < deadline);
        expect(attributes.map_state == IsViewable, "Live canvas test window must be mapped");
        test_live_source(app, root);
        test_composer_and_state(app, root);
        std::cout << "Live source, language updates, native composer and reload-state preservation "
                     "passed.\n";
    }
};

int main()
{
    if (!std::getenv("DISPLAY"))
        return 77;
    const auto root =
        std::filesystem::temp_directory_path() / ("live-canvas-" + std::to_string(getpid()));
    try {
        std::filesystem::create_directories(root);
        std::ofstream(root / "main.cpp") << "#include \"widget.h\"\nint main() { return 0; }\n";
        std::ofstream(root / "widget.h") << "#pragma once\n";
        setenv("SOURCE_CANVAS_PROJECT_ROOT", root.c_str(), 1);
        setenv("SOURCE_CANVAS_DISABLE_AGENT", "1", 1);
        setenv("SOURCE_CANVAS_AUTO_BUILD", "0", 1);
        {
            Application app;
            ApplicationTestAccess::run(app, root);
        }
        std::filesystem::remove_all(root);
    } catch (const std::exception& error) {
        std::filesystem::remove_all(root);
        std::cerr << error.what() << '\n';
        return 1;
    }
}

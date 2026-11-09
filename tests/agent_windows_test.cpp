#include "application.h"
#include <unistd.h>

namespace {
void expect(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}
} // namespace
struct ApplicationTestAccess {
    static Vec2 source_point(Application& app)
    {
        auto& panel = app.panels.front();
        PangoRectangle glyph{};
        pango_layout_index_to_pos(panel.lines[1], 5, &glyph);
        app.initial_view = false;
        app.camera.zoom = 1;
        app.camera.center = {panel.bounds.origin.x + 24 +
                                 static_cast<double>(glyph.x + glyph.width / 2) / PANGO_SCALE,
                             panel.bounds.origin.y + app.header_height + app.line_height + 10};
        app.zoom_animation.cancel(app.camera);
        app.mouse_position = app.viewport().center();
        return app.mouse_position;
    }
    static void await_symbols(Application& app)
    {
        const auto deadline = Application::Clock::now() + std::chrono::seconds(8);
        while (app.panels.front().symbols.empty() && Application::Clock::now() < deadline) {
            for (const auto& message : app.language_server->take_messages())
                app.handle_message(message);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        expect(!app.panels.front().symbols.empty(),
               "The context fixture must have real language-server symbols");
    }
    static void test_spawn_and_context(Application& app)
    {
        await_symbols(app);
        const auto pointer = source_point(app);
        auto context = app.agent_context(pointer);
        expect(context["path"] == "main.cpp" && context["line"] == 2 &&
                   context["symbol"] == "Camera::value",
               ("Spawn context must capture the pointed file, line and enclosing symbol: " +
                context.dump())
                   .c_str());
        app.text_selection.surface = TextSurface::Source;
        app.text_selection.key = app.panels.front().path;
        app.text_selection.range.set(app.panels.front().source);
        const auto begin = app.panels.front().source.find("value");
        app.text_selection.range.place(begin);
        app.text_selection.range.place(begin + 5, true);
        context = app.agent_context({0, 0});
        expect(context["selected_text"] == "value" && context["line"] == 2 &&
                   context["path"] == "main.cpp",
               "Selected source must retain its file and line when spawning on blank canvas");
        app.chat->conversation.thread_id = "original-thread";
        app.chat->input.set("Original draft λ");
        const auto original = app.chat.get();
        const Vec2 location{-140, 260};
        const auto camera = app.camera;
        app.spawn_agent_window(location, context);
        expect(app.chat.get() != original && app.chat_windows.size() == 1 &&
                   app.chat->bounds.origin.x == location.x &&
                   app.chat->bounds.origin.y == location.y,
               "The new agent must be placed exactly at the requested world position");
        expect(app.camera.center.x == camera.center.x && app.camera.center.y == camera.center.y &&
                   app.camera.zoom == camera.zoom,
               "Spawning at the pointer must preserve the user's camera position and zoom");
        expect(app.chat->conversation.fresh && app.chat->conversation.thread_id.empty() &&
                   app.chat_windows.front()->input.text == "Original draft λ",
               "Spawning must preserve the old card and give the new card an independent draft and "
               "thread");
        expect(
            app.chat->conversation.canvas_context.find("Camera") != std::string::npos &&
                app.chat->conversation.entries.front().role == "Context",
            "Context must be visible locally and attached without manufacturing a human message");
        app.chat->input.set("New card draft");
        app.draw();
        const auto old_point = app.camera.world_to_screen(
            {original->bounds.origin.x + 12, original->bounds.origin.y + 12}, app.viewport());
        expect(
            app.activate_chat(old_point) && app.chat.get() == original,
            "Clicking an existing agent must activate that card rather than route to the new one");
        expect(app.chat->input.text == "Original draft λ" &&
                   app.chat_windows.front()->input.text == "New card draft",
               "Activation must not mix independent drafts");
    }
    static void test_reload_state(Application& app)
    {
        app.chat->disabled = false;
        app.save_session();
        app.chat->input.set("");
        app.chat_windows.clear();
        app.load_session();
        expect(app.chat->input.text == "Original draft λ" && app.chat_windows.size() == 1 &&
                   app.chat_windows.front()->input.text == "New card draft" &&
                   app.chat_windows.front()->conversation.fresh &&
                   app.chat_windows.front()->context["path"] == "main.cpp",
               "Hot reload must preserve every card, independent draft and source context");
        app.chat->disabled = true;
        app.chat_windows.front()->conversation.questions.push_back(
            {1, "approval", "Approval", lsp::Json::object()});
        expect(app.agents_pending(), "Pending requests in an inactive card must defer reload");
        app.chat_windows.front()->conversation.questions.clear();
        expect(!app.agents_pending(), "Reload must become available once all cards are settled");
    }
    static void test_menu_and_close(Application& app)
    {
        app.chat->visible = false;
        for (auto& card : app.chat_windows)
            card->visible = false;
        const auto pointer = source_point(app);
        const auto world = app.camera.screen_to_world(pointer, app.viewport());
        app.show_agent_menu(pointer);
        app.draw();
        XEvent click{};
        click.type = ButtonPress;
        click.xbutton.button = Button1;
        click.xbutton.x = static_cast<int>(app.popup_bounds.origin.x + 18);
        click.xbutton.y = static_cast<int>(app.popup_bounds.origin.y + 18);
        app.handle_button(click);
        expect(app.chat_windows.size() == 2 && app.chat->visible &&
                   app.chat->bounds.origin.x == world.x && app.chat->bounds.origin.y == world.y,
               "The right-click action must spawn at the captured position rather than the menu "
               "click");
        app.close_agent_window();
        expect(!app.chat->visible && app.chat->input.text.empty(),
               "Closing a new card must hide it without deleting other drafts");
        XEvent key{};
        key.type = KeyPress;
        key.xkey.display = app.display;
        key.xkey.window = app.window;
        key.xkey.keycode = XKeysymToKeycode(app.display, XK_a);
        key.xkey.state = ControlMask | ShiftMask;
        app.handle_key(key);
        expect(app.chat->visible && app.chat_windows.size() == 3,
               "The new-agent shortcut must work even when every old card is closed");
    }
    static void run(Application& app)
    {
        expect(app.initialize(), "The multi-agent canvas must initialize");
        test_spawn_and_context(app);
        test_reload_state(app);
        test_menu_and_close(app);
        app.draw();
    }
};
int main()
{
    if (!std::getenv("DISPLAY"))
        return 77;
    const auto root = std::filesystem::temp_directory_path() /
                      ("canvas-agent-windows-" + std::to_string(getpid()));
    int result = 0;
    try {
        std::filesystem::create_directories(root);
        std::ofstream(root / "main.cpp") << "struct Camera {\n int value = 42;\n};\n";
        setenv("SOURCE_CANVAS_PROJECT_ROOT", root.c_str(), 1);
        setenv("SOURCE_CANVAS_DISABLE_AGENT", "1", 1);
        setenv("SOURCE_CANVAS_DISABLE_INTENT", "1", 1);
        setenv("SOURCE_CANVAS_AUTO_BUILD", "0", 1);
        setenv("SOURCE_CANVAS_STATE_FILE", (root / "session.json").c_str(), 1);
        unsetenv("SOURCE_CANVAS_DISABLE_SESSION");
        Application app;
        ApplicationTestAccess::run(app);
        std::cout << "Agent placement, context, independent conversations and reload-state checks "
                     "passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    std::filesystem::remove_all(root);
    return result;
}

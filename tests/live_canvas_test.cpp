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
    static void click_word(Application& app, const SourcePanel& source, int line,
                           const std::string& word)
    {
        PangoRectangle glyph{};
        const auto byte = source.text[line].find(word);
        pango_layout_index_to_pos(source.lines[line], static_cast<int>(byte), &glyph);
        const auto pointer = app.camera.world_to_screen(
            {source.bounds.origin.x + 24 + static_cast<double>(glyph.x) / PANGO_SCALE + 2,
             source.bounds.origin.y + app.header_height + (line + 0.5) * app.line_height},
            app.viewport());
        XEvent press{};
        press.type = ButtonPress;
        press.xbutton.button = Button1;
        press.xbutton.x = static_cast<int>(pointer.x);
        press.xbutton.y = static_cast<int>(pointer.y);
        app.mouse_position = pointer;
        app.handle_button(press);
        press.type = ButtonRelease;
        press.xbutton.state = Button1Mask;
        app.handle_event(press);
    }
    static void test_marker_parts()
    {
        const ConnectionEnd first{"file:///a.cpp", 4, 0, 3}, second{"file:///a.cpp", 90, 0, 3};
        const std::vector<ConnectionLink> links{{first, "draw", "a.cpp", false, 1},
                                                {second, "resize", "a.cpp", false, 3}};
        ConnectionMarker marker{{0, 1}, {{100, 50}, {400, 24}}, "", marker_parts(links, {0, 1})};
        expect(marker.parts.size() == 3 && marker.parts[1].text == "resize," &&
                   marker.parts[1].link == 1 && marker.parts[2].text == "draw" &&
                   marker.parts[2].link == 0,
               "A marker must name each function as its own part, most used first");
        double right = 12;
        for (auto& part : marker.parts) {
            part.offset = right;
            part.width = 60;
            right += 70;
        }
        const auto* pointed = marker_part_at(marker, {100 + marker.parts[2].offset + 5, 60});
        expect(pointed && pointed->link == 0 && !marker_part_at(marker, {100 + right + 40, 60}) &&
                   !marker_part_at(marker, {130, 10}),
               "Pointing at a name in a marker must pick that use, not the first one");
    }
    static void test_symbol_connections(Application& app, const std::filesystem::path&)
    {
        const auto& caller = panel(app, "main.cpp");
        const auto& target = panel(app, "widget.h");
        wait_for(app, [&] { return app.language_ready && !caller.semantics.empty(); });
        expect(app.connection_links.empty() && !app.connection_subject.active,
               "Nothing is connected until a symbol is chosen");
        app.initial_view = false;
        app.camera.zoom = 1;
        app.camera.center = {caller.bounds.origin.x + 200, caller.bounds.origin.y + 60};
        app.zoom_animation.cancel(app.camera);
        click_word(app, caller, 1, "compute");
        expect(app.connection_subject.active && app.connection_subject.name == "compute",
               "A click on a symbol must make it the subject of the arrows");
        wait_for(app, [&] {
            return !app.connection_requests.first && !app.connection_requests.second &&
                   !app.connection_links.empty();
        });
        const auto defined = std::find_if(app.connection_links.begin(), app.connection_links.end(),
                                          [](const auto& link) { return link.defines; });
        expect(defined != app.connection_links.end() && defined->at.uri == target.uri &&
                   target.text[defined->at.line].substr(
                       defined->at.begin, defined->at.end - defined->at.begin) == "compute",
               "The definition must be found, on the word itself, across Unicode prefixes");
        expect(app.popup_text.empty() &&
                   std::abs(app.camera.center.x - caller.bounds.origin.x - 200) < 0.001,
               "A click must draw arrows where you are, not jump away or open a list");
        app.draw();
        expect(glGetError() == GL_NO_ERROR &&
                   (!app.connection_markers.empty() || app.connection_mesh.vertices > 0),
               "Every link must end at a highlighted word or at a marker on the window edge");
        // Far from everything, the subject and its definition are both markers to fly back to.
        app.camera.center.x += 60000;
        app.zoom_animation.cancel(app.camera);
        app.draw();
        expect(app.connection_words.empty() && !app.connection_markers.empty() &&
                   app.connection_mesh.vertices == 0,
               "With every end out of view only markers remain, and no arrow runs off screen");
        XEvent escape{};
        escape.type = KeyPress;
        escape.xkey.display = app.display;
        escape.xkey.keycode = XKeysymToKeycode(app.display, XK_Escape);
        app.handle_event(escape);
        app.draw();
        expect(!app.connection_subject.active && app.connection_links.empty() &&
                   app.connection_markers.empty(),
               "Escape must put the arrows away");
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
    static XEvent button(unsigned number, Vec2 point)
    {
        XEvent event{};
        event.type = ButtonPress;
        event.xbutton.button = number;
        event.xbutton.x = static_cast<int>(point.x);
        event.xbutton.y = static_cast<int>(point.y);
        return event;
    }
    static void move_pointer(Application& app, Vec2 world)
    {
        const auto screen = app.camera.world_to_screen(world, app.viewport());
        XEvent motion{};
        motion.type = MotionNotify;
        motion.xmotion.x = static_cast<int>(screen.x);
        motion.xmotion.y = static_cast<int>(screen.y);
        app.handle_motion(motion);
    }
    static void test_scrollbar_drag(Application& app, double zoom)
    {
        app.camera.zoom = zoom;
        app.zoom_animation.cancel(app.camera);
        app.chat->follow = false;
        app.chat->scroll = 400;
        app.draw();
        const auto thumb = app.chat->scrollbar_thumb();
        const auto track = app.chat->scrollbar_track();
        const auto world =
            Vec2{thumb.origin.x + thumb.size.x / 2, thumb.origin.y + thumb.size.y / 3};
        const auto point = app.camera.world_to_screen(world, app.viewport());
        const auto scroll = app.chat->scroll;
        const auto camera = app.camera;
        const auto card = app.chat->bounds.origin;
        app.handle_button(button(Button1, point));
        expect(app.chat->scrolling && app.drag_button == Button1 &&
                   app.active_cursor == app.normal_cursor &&
                   std::abs(app.chat->scroll - scroll) < 0.001,
               "Grabbing the scrollbar must keep its offset and use a normal cursor");
        move_pointer(app, {track.origin.x + 90, track.origin.y + track.size.y + 100});
        expect(app.chat->scroll == app.chat->content_height - app.chat->body.size.y &&
                   app.chat->follow,
               "Scrollbar drags must stay captured outside the card and clamp to the bottom");
        expect(app.camera.center.x == camera.center.x && app.camera.center.y == camera.center.y &&
                   app.camera.zoom == camera.zoom && app.chat->bounds.origin.x == card.x &&
                   app.chat->bounds.origin.y == card.y,
               "Dragging the scrollbar must not move the camera or conversation card");
        const auto bottom = app.chat->scroll;
        const auto anchor = app.camera.screen_to_world(app.mouse_position, app.viewport());
        app.handle_button(button(Button4, app.mouse_position));
        app.zoom_animation.advance(app.camera, app.viewport(), 0.04);
        const auto anchored = app.camera.screen_to_world(app.mouse_position, app.viewport());
        expect(app.camera.zoom > camera.zoom && app.chat->scroll == bottom &&
                   std::abs(anchor.x - anchored.x) < 0.001 &&
                   std::abs(anchor.y - anchored.y) < 0.001,
               "Wheel zoom must remain anchored and independent while dragging the scrollbar");
        app.zoom_animation.cancel(app.camera);
        move_pointer(app, {track.origin.x, track.origin.y - 100});
        expect(app.chat->scroll == 0 && !app.chat->follow,
               "Dragging to the top must clamp and stop following new output");
        app.handle_release(button(Button1, app.mouse_position));
        expect(!app.chat->scrolling && !app.drag_button,
               "Releasing the scrollbar must finish the captured drag");
    }
    static void test_conversation_navigation(Application& app)
    {
        for (int i = 0; i < 40; ++i)
            app.chat->conversation.entries.push_back(
                {"scroll-entry-" + std::to_string(i), "fixture-turn", "Agent",
                 "A conversation entry that keeps enough history to scroll.", ""});
        app.focus_chat();
        app.chat->follow = false;
        app.chat->scroll = 200;
        app.draw();
        for (const auto world :
             {app.chat->body.origin, app.chat->editor.origin, app.chat->scrollbar_track().origin}) {
            const auto point =
                app.camera.world_to_screen({world.x + 6, world.y + 6}, app.viewport());
            const auto scroll = app.chat->scroll;
            const auto zoom = app.camera.zoom;
            const auto wheel = button(Button4, point);
            const Vec2 pointer{static_cast<double>(wheel.xbutton.x),
                               static_cast<double>(wheel.xbutton.y)};
            const auto anchor = app.camera.screen_to_world(pointer, app.viewport());
            app.handle_button(wheel);
            app.zoom_animation.advance(app.camera, app.viewport(), 0.04);
            const auto anchored = app.camera.screen_to_world(pointer, app.viewport());
            expect(app.camera.zoom > zoom && app.chat->scroll == scroll &&
                       std::abs(anchor.x - anchored.x) < 0.001 &&
                       std::abs(anchor.y - anchored.y) < 0.001,
                   "Wheel over messages, the composer and scrollbar must zoom without scrolling");
            app.zoom_animation.cancel(app.camera);
            app.handle_button(button(Button5, point));
            app.zoom_animation.advance(app.camera, app.viewport(), 0.04);
            expect(app.chat->scroll == scroll,
                   "Zooming out must also preserve conversation scroll");
            app.zoom_animation.cancel(app.camera);
        }
        test_scrollbar_drag(app, 0.65);
        test_scrollbar_drag(app, 1.4);
        app.chat->scroll = app.chat->content_height - app.chat->body.size.y;
        app.chat->follow = true;
        app.chat->conversation.entries.push_back(
            {"new-output", "fixture-turn", "Agent", "New output", ""});
        app.draw();
        expect(app.chat->scroll == app.chat->content_height - app.chat->body.size.y,
               "Returning the scrollbar to the bottom must follow new agent output");
    }
    static void test_composer_and_state(Application& app, const std::filesystem::path& root)
    {
        app.chat->disabled = false;
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
    }
    static void run(Application& app, const std::filesystem::path& root, bool symbols)
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
        if (symbols) {
            test_marker_parts();
            test_symbol_connections(app, root);
            std::cout << "Arrows for a chosen symbol, markers out of view and dismissal passed.\n";
            return;
        }
        test_live_source(app, root);
        test_conversation_navigation(app);
        test_composer_and_state(app, root);
        std::cout << "Live source, language updates, native composer and reload-state preservation "
                     "passed.\n";
    }
};

int main(int argc, char** argv)
{
    if (!std::getenv("DISPLAY"))
        return 77;
    const auto root =
        std::filesystem::temp_directory_path() / ("live-canvas-" + std::to_string(getpid()));
    const auto launch_directory = std::filesystem::current_path();
    const bool symbols = argc > 1 && std::string(argv[1]) == "--symbols";
    try {
        std::filesystem::create_directories(root);
        std::ofstream(root / "main.cpp")
            << (symbols ? "#include \"widget.h\"\nint local() { return compute(1); }\n"
                          "int main() { return local() + static_cast<int>(compute(1.5)); }\n"
                        : "#include \"widget.h\"\nint main() { return 0; }\n");
        std::ofstream(root / "widget.h")
            << (symbols
                    ? "#pragma once\n/* 🌍 */ inline int compute(int value) { return value + 1; }\n"
                      "inline double compute(double value) { return value + 0.5; }\n"
                    : "#pragma once\n");
        std::filesystem::current_path(root);
        unsetenv("SOURCE_CANVAS_PROJECT_ROOT");
        setenv("SOURCE_CANVAS_DISABLE_AGENT", "1", 1);
        {
            Application app(root.string());
            ApplicationTestAccess::run(app, root, symbols);
        }
        std::filesystem::current_path(launch_directory);
        std::filesystem::remove_all(root);
    } catch (const std::exception& error) {
        std::filesystem::current_path(launch_directory);
        std::filesystem::remove_all(root);
        std::cerr << error.what() << '\n';
        return 1;
    }
}

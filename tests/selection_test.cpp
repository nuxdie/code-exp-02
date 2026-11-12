#include "application.h"
#include <X11/Xatom.h>
#include <unistd.h>

namespace {
void expect(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}
} // namespace
struct ApplicationTestAccess {
    static XEvent mouse(unsigned button, Vec2 pointer)
    {
        XEvent event{};
        event.type = ButtonPress;
        event.xbutton.button = button;
        event.xbutton.x = static_cast<int>(pointer.x);
        event.xbutton.y = static_cast<int>(pointer.y);
        return event;
    }
    static Vec2 glyph(PangoLayout* layout, std::size_t byte, Vec2 origin, double zoom)
    {
        PangoRectangle position{};
        pango_layout_index_to_pos(layout, static_cast<int>(byte), &position);
        return {std::ceil(origin.x + static_cast<double>(position.x) / PANGO_SCALE * zoom),
                origin.y +
                    static_cast<double>(position.y + position.height / 2) / PANGO_SCALE * zoom};
    }
    static void drag(Application& app, Vec2 first, Vec2 last)
    {
        const auto camera = app.camera;
        app.handle_button(mouse(Button1, first));
        if (!app.text_selection.dragging || app.active_cursor != app.text_cursor)
            std::cerr << "Selection target=" << static_cast<int>(app.text_at(first).surface)
                      << " drag=" << app.drag_button << " point=" << first.x << "," << first.y
                      << '\n';
        expect(app.text_selection.dragging && app.active_cursor == app.text_cursor,
               "Text must capture a selection drag with a text cursor");
        XEvent move{};
        move.type = MotionNotify;
        move.xmotion.x = static_cast<int>(last.x);
        move.xmotion.y = static_cast<int>(last.y);
        app.handle_motion(move);
        app.draw();
        app.handle_release(mouse(Button1, last));
        expect(!app.text_selection.dragging && !app.drag_button &&
                   app.camera.center.x == camera.center.x && app.camera.center.y == camera.center.y,
               "Selecting text must release capture without panning the camera");
    }
    static void key(Application& app, KeySym symbol, unsigned modifiers)
    {
        XEvent event{};
        event.type = KeyPress;
        event.xkey.display = app.display;
        event.xkey.window = app.window;
        event.xkey.keycode = XKeysymToKeycode(app.display, symbol);
        event.xkey.state = modifiers;
        app.handle_key(event);
    }
    static void test_source(Application& app)
    {
        auto& panel = app.panels.front();
        app.initial_view = false;
        app.camera.zoom = 1;
        app.camera.center = {panel.bounds.origin.x + 200, panel.bounds.origin.y + 100};
        app.zoom_animation.cancel(app.camera);
        app.draw();
        const auto origin = app.camera.world_to_screen(
            {panel.bounds.origin.x + 24, panel.bounds.origin.y + app.header_height},
            app.viewport());
        const auto first = glyph(panel.lines[0].get(), 4, origin, 1);
        const auto last =
            glyph(panel.lines[1].get(), 10, {origin.x, origin.y + app.line_height}, 1);
        drag(app, first, last);
        expect(app.selected_text() == "first = 1;\nint second",
               "Source selections must copy exact multiline source text");
        key(app, XK_c, ControlMask);
        expect(app.clipboard_text == app.selected_text(),
               "Ctrl+C must copy the selected source, not the composer");
        key(app, XK_a, ControlMask);
        expect(app.selected_text() == panel.source, "Ctrl+A must select the active source card");
        key(app, XK_Escape, 0);
        expect(app.selected_text().empty(), "Escape must clear a selection");
    }
    static void test_popup(Application& app)
    {
        app.show_popup("Readable **prose** and `code` with λ.\n\nAnother paragraph.", {30, 30},
                       true);
        app.draw();
        const Vec2 origin{app.popup_bounds.origin.x + 12, app.popup_bounds.origin.y + 12};
        const auto first = glyph(app.popup_layout.get(), 0, origin, 1);
        const auto last = glyph(app.popup_layout.get(), app.popup_text.find(" with"), origin, 1);
        drag(app, first, last);
        expect(app.selected_text() == "Readable prose and code",
               "Popup selection must copy visible text across style changes");
        expect(!app.popup_text.empty(), "Selecting documentation must keep its popup visible");
        key(app, XK_c, ControlMask);
        expect(app.clipboard_text == "Readable prose and code",
               "Popup text must be available to clipboard shortcuts");
        app.hide_popup();
        app.text_selection = {};
    }
    static void test_chat(Application& app)
    {
        app.chat->conversation.entries.push_back(
            {"selection-message", "turn", "You", "Select ligatures != and Unicode λ 🌍.", ""});
        app.focus_chat();
        app.draw();
        const auto& view = app.chat->messages.at("selection-message");
        const auto hit = std::ranges::find_if(
            app.chat->hits, [](const auto& h) { return h.kind == ChatHit::Kind::Message; });
        expect(hit != app.chat->hits.end(), "The conversation message must be visible");
        const auto origin = app.camera.world_to_screen(hit->bounds.origin, app.viewport());
        const auto begin = view.document.text.find("Select");
        drag(app, glyph(view.layout.get(), begin, origin, app.camera.zoom),
             glyph(view.layout.get(), view.document.text.size(), origin, app.camera.zoom));
        expect(app.selected_text() == "Select ligatures != and Unicode λ 🌍.",
               "Message selection must respect Unicode and ligature shaping at canvas zoom");
        app.chat->input.set("A draft with λ");
        app.chat->focused = true;
        app.draw();
        const auto input_origin =
            app.camera.world_to_screen({app.chat->editor.origin.x + 12,
                                        app.chat->editor.origin.y + 10 - app.chat->editor_scroll},
                                       app.viewport());
        drag(app, glyph(app.chat->composer.layout.get(), 2, input_origin, app.camera.zoom),
             glyph(app.chat->composer.layout.get(), 7, input_origin, app.camera.zoom));
        expect(app.chat->input.selected() == "draft" && app.chat->input.text == "A draft with λ",
               "Dragging in the composer must select text without modifying the draft");
    }
    static void expect_opened(const std::filesystem::path& log, const std::string& url)
    {
        const auto deadline = Application::Clock::now() + 2s;
        while (Application::Clock::now() < deadline) {
            std::ifstream input(log);
            const std::string output{std::istreambuf_iterator<char>(input), {}};
            if (output == "1\n" + url + "\n") {
                std::filesystem::remove(log);
                return;
            }
            std::this_thread::sleep_for(5ms);
        }
        expect(false, "Clicking a URL must pass the exact destination as one browser argument");
    }
    static void click_link(Application& app, Vec2 point, const std::string& url,
                           const std::filesystem::path& log)
    {
        app.mouse_position = point;
        app.update_cursor();
        expect(app.active_cursor == app.link_cursor, "Links must show a hand cursor on hover");
        app.handle_button(mouse(Button1, point));
        expect(!std::filesystem::exists(log),
               "Pressing a link must wait for release before opening");
        app.handle_release(mouse(Button1, point));
        expect_opened(log, url);
    }
    static void test_links(Application& app)
    {
        const auto log = std::filesystem::path(app.workspace_root) / "opened-url";
        const std::string url = "https://example.com/code?a=1&b=two";
        auto& panel = app.panels.front();
        app.chat->visible = false;
        app.camera.zoom = 0.8;
        app.camera.center = {panel.bounds.origin.x + 200, panel.bounds.origin.y + 100};
        app.zoom_animation.cancel(app.camera);
        app.draw();
        const auto origin = app.camera.world_to_screen(
            {panel.bounds.origin.x + 24,
             panel.bounds.origin.y + app.header_height + 2 * app.line_height},
            app.viewport());
        const auto first = glyph(panel.lines[2].get(), 3, origin, app.camera.zoom);
        const auto last =
            glyph(panel.lines[2].get(), panel.text[2].size(), origin, app.camera.zoom);
        drag(app, first, last);
        expect(app.selected_text() == url && !std::filesystem::exists(log),
               "Dragging over a source URL must select it without opening the browser");
        click_link(app, glyph(panel.lines[2].get(), 10, origin, app.camera.zoom), url, log);

        app.text_selection = {};
        app.chat->conversation.entries.clear();
        app.chat->conversation.entries.push_back(
            {"links-message", "turn", "Assistant",
             "λ [Browser](https://example.com/docs?a=1&b=2) and [Source](main.cpp:2:5)\n"
             "Bare https://example.com/plain.\n```cpp\n"
             "auto url = \"https://example.com/fenced\";\n```",
             ""});
        app.focus_chat();
        app.draw();
        const auto& view = app.chat->messages.at("links-message");
        const auto hit = std::ranges::find_if(
            app.chat->hits, [](const auto& h) { return h.kind == ChatHit::Kind::Message; });
        expect(hit != app.chat->hits.end(), "The link message must be visible");
        const auto message_origin = app.camera.world_to_screen(hit->bounds.origin, app.viewport());
        for (const auto& label : {std::string("Browser"), std::string("https://example.com/plain"),
                                  std::string("https://example.com/fenced")}) {
            const auto point = glyph(view.layout.get(), view.document.text.find(label) + 2,
                                     message_origin, app.camera.zoom);
            click_link(app, point, label == "Browser" ? "https://example.com/docs?a=1&b=2" : label,
                       log);
        }
        const auto local = glyph(view.layout.get(), view.document.text.find("Source") + 2,
                                 message_origin, app.camera.zoom);
        const auto chat_screen =
            app.camera.world_to_screen(app.chat->bounds.origin, app.viewport());
        auto other = std::make_unique<ChatCanvas>(app.workspace_root);
        other->bounds.origin = {12000, 14000};
        app.chat_windows.push_back(std::move(other));
        app.handle_button(mouse(Button1, local));
        app.handle_release(mouse(Button1, local));
        expect(app.focused_line && app.focused_line->line == 1 && app.focused_line->byte == 4,
               "Chat file links must navigate to the requested line and column");
        const auto moved_screen =
            app.camera.world_to_screen(app.chat->bounds.origin, app.viewport());
        expect(std::hypot(moved_screen.x - chat_screen.x, moved_screen.y - chat_screen.y) < 0.001,
               "The clicked chat must follow source navigation at the same screen position");
        expect(app.chat_windows.back()->bounds.origin.x == 12000 &&
                   app.chat_windows.back()->bounds.origin.y == 14000,
               "Following a link must leave other chat windows in place");
        app.camera.center.x += 30;
        const auto shortcut_before =
            app.camera.world_to_screen(app.chat->bounds.origin, app.viewport());
        app.draw();
        const auto& updated = app.chat->messages.at("links-message");
        const auto updated_hit = std::ranges::find_if(
            app.chat->hits, [](const auto& h) { return h.kind == ChatHit::Kind::Message; });
        const auto shortcut_point =
            glyph(updated.layout.get(), updated.document.text.find("Source") + 2,
                  app.camera.world_to_screen(updated_hit->bounds.origin, app.viewport()),
                  app.camera.zoom);
        expect(app.open_document_link(shortcut_point), "The shortcut must open chat source links");
        const auto shortcut_screen =
            app.camera.world_to_screen(app.chat->bounds.origin, app.viewport());
        expect(std::hypot(shortcut_screen.x - shortcut_before.x,
                          shortcut_screen.y - shortcut_before.y) < 0.001,
               "Shortcut navigation must also bring the clicked chat along");
        expect(app.open_link(lsp::file_uri(
                   (std::filesystem::path(app.workspace_root) / "main.cpp").string())),
               "File URI links must continue to open source cards");
    }
    static void run(Application& app)
    {
        expect(app.initialize(), "Selection canvas must initialize");
        XWindowAttributes attributes{};
        for (int i = 0; i < 200; ++i) {
            XGetWindowAttributes(app.display, app.window, &attributes);
            if (attributes.map_state == IsViewable)
                break;
            std::this_thread::sleep_for(5ms);
        }
        expect(attributes.map_state == IsViewable, "The selection test window must be mapped");
        test_source(app);
        test_popup(app);
        test_chat(app);
        test_links(app);
        std::cout << "Source, popup, Unicode message and composer selection checks passed.\n";
    }
};
int main()
{
    if (!std::getenv("DISPLAY"))
        return 77;
    const auto root =
        std::filesystem::temp_directory_path() / ("canvas-selection-" + std::to_string(getpid()));
    int result = 0;
    try {
        std::filesystem::create_directories(root);
        std::ofstream(root / "main.cpp") << "int first = 1;\nint second = 2;\n"
                                            "// https://example.com/code?a=1&b=two\n";
        std::filesystem::create_directories(root / "bin");
        const auto opener = root / "bin/xdg-open";
        std::ofstream(opener) << "#!/bin/sh\nprintf '%s\\n' \"$#\" \"$1\" > \"$CANVAS_LINK_LOG\"\n";
        std::filesystem::permissions(opener, std::filesystem::perms::owner_all);
        const auto path = (root / "bin").string() + ":" + std::getenv("PATH");
        setenv("PATH", path.c_str(), 1);
        setenv("CANVAS_LINK_LOG", (root / "opened-url").c_str(), 1);
        setenv("SOURCE_CANVAS_PROJECT_ROOT", root.c_str(), 1);
        setenv("SOURCE_CANVAS_DISABLE_AGENT", "1", 1);
        setenv("SOURCE_CANVAS_DISABLE_SESSION", "1", 1);
        setenv("SOURCE_CANVAS_DISABLE_INTENT", "1", 1);
        Application app;
        ApplicationTestAccess::run(app);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    std::filesystem::remove_all(root);
    return result;
}

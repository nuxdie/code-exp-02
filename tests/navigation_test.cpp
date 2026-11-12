#include "application.h"
#include <unistd.h>

namespace {
void expect(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}

bool same(const Camera& a, const Camera& b)
{
    return std::abs(a.center.x - b.center.x) < 1e-8 && std::abs(a.center.y - b.center.y) < 1e-8 &&
           std::abs(a.zoom - b.zoom) < 1e-8;
}
} // namespace

struct ApplicationTestAccess {
    static void key(Application& app, KeySym symbol, unsigned modifiers = 0)
    {
        XEvent event{};
        event.type = KeyPress;
        event.xkey.display = app.display;
        event.xkey.keycode = XKeysymToKeycode(app.display, symbol);
        event.xkey.state = modifiers;
        app.handle_key(event);
    }

    static void pan(Application& app, Vec2 delta)
    {
        app.remember_view(true);
        app.camera.pan(delta);
    }

    static void button(Application& app, unsigned value)
    {
        XEvent event{};
        event.type = ButtonPress;
        event.xbutton.button = value;
        event.xbutton.x = 100;
        event.xbutton.y = 100;
        app.handle_button(event);
    }

    static void reset(Application& app)
    {
        app.pending_view.reset();
        app.navigation_history.clear();
        app.navigation_forward.clear();
        app.camera = {{-10000, -10000}, 0.75};
        app.zoom_animation.cancel(app.camera);
        app.initial_view = false;
        app.chat->focused = false;
        app.close_search();
        app.hide_popup();
    }

    static void test_keys(Application& app)
    {
        reset(app);
        const auto start = app.camera;
        key(app, XK_Left, Mod1Mask);
        key(app, XK_Right, Mod1Mask);
        expect(same(app.camera, start),
               "Empty back and forward must never fall through to panning");
        pan(app, {-40, 0});
        pan(app, {-40, 0});
        const auto panned = app.camera;
        key(app, XK_Left, Mod1Mask);
        expect(same(app.camera, start), "Repeated pan steps must form one history step");
        key(app, XK_Right, Mod1Mask);
        expect(same(app.camera, panned), "Alt+Right must restore the exact pan destination");
        key(app, XK_Right, Mod1Mask);
        expect(same(app.camera, panned), "Forward at the newest position must do nothing");
        key(app, XK_Left, Mod1Mask);
        pan(app, {0, -40});
        expect(!app.go_forward(), "New movement after back must discard the forward branch");

        reset(app);
        pan(app, {-40, 0});
        const auto first = app.camera;
        app.navigation_input -= 1s;
        pan(app, {0, -40});
        expect(app.go_back() && same(app.camera, first), "A pause must separate gestures");
        expect(app.go_back() && same(app.camera, start),
               "Both separate gestures must be reachable");
    }

    static void test_drag_and_zoom(Application& app)
    {
        reset(app);
        const auto start = app.camera;
        button(app, Button2);
        expect(app.drag_button == Button2, "Middle-drag must begin a pan");
        XEvent motion{};
        motion.type = MotionNotify;
        motion.xmotion.x = 120;
        motion.xmotion.y = 130;
        app.handle_motion(motion);
        app.navigation_input -= 1s;
        motion.xmotion.x = 180;
        app.handle_motion(motion);
        XEvent release{};
        release.type = ButtonRelease;
        release.xbutton.button = Button2;
        app.handle_event(release);
        const auto dragged = app.camera;
        expect(app.navigation_history.size() == 1 && app.go_back() && same(app.camera, start),
               "A whole drag must form one history step even during slow motion");
        button(app, 9);
        expect(same(app.camera, dragged), "Mouse forward must restore a dragged view");
        button(app, 8);
        expect(same(app.camera, start), "Mouse back must restore the drag origin");
        button(app, Button2);
        app.handle_event(release);
        expect(app.go_forward() && same(app.camera, dragged),
               "Clicking without moving must preserve the forward branch");

        reset(app);
        button(app, Button4);
        app.zoom_animation.advance(app.camera, app.viewport(), 0.02);
        button(app, Button4);
        for (int i = 0; i < 200; ++i)
            app.zoom_animation.advance(app.camera, app.viewport(), 0.02);
        const auto zoomed = app.camera;
        expect(zoomed.zoom > start.zoom && app.go_back() && same(app.camera, start),
               "A wheel burst and its animation must form one history step");
        expect(!app.zoom_animation.active(app.camera) && app.go_forward() &&
                   same(app.camera, zoomed),
               "Forward must restore zoom and its pointer-anchored center exactly");
        button(app, Button5);
        app.zoom_animation.advance(app.camera, app.viewport(), 0.02);
        expect(app.go_back() && same(app.camera, zoomed),
               "Back during smooth zoom must cancel the animation and restore its origin");
        expect(!app.zoom_animation.advance(app.camera, app.viewport(), 1),
               "Cancelled animation must not move a restored view");
    }

    static void test_destinations(Application& app)
    {
        reset(app);
        const auto start = app.camera;
        key(app, XK_f);
        const auto fitted = app.camera;
        app.remember_view();
        app.camera.pan({-300, 0});
        expect(app.go_back() && same(app.camera, fitted),
               "Fit and a later move must be separate steps");
        expect(app.go_back() && same(app.camera, start), "Fit must retain the previous camera");
        key(app, XK_Left, Mod1Mask);
        expect(same(app.camera, start), "Back at the oldest position must do nothing");

        reset(app);
        const auto& panel = app.panels.front();
        const lsp::Json location{{"uri", panel.uri},
                                 {"range", {{"start", {{"line", 0}, {"character", 0}}}}}};
        expect(app.jump_to(location), "A code jump must succeed");
        const auto jumped = app.camera;
        app.focus_chat();
        const auto conversation = app.camera;
        key(app, XK_Left, Mod1Mask);
        expect(same(app.camera, jumped),
               "Navigation must work with the conversation editor focused");
        key(app, XK_Left, Mod1Mask);
        expect(same(app.camera, start), "A code jump must preserve its origin");
        app.open_search();
        key(app, XK_Right, Mod1Mask);
        expect(same(app.camera, jumped), "Camera history shortcuts must work while search is open");
        app.close_search();
        expect(app.jump_to(location) && app.go_forward() && same(app.camera, conversation),
               "Jumping to the current location must preserve the forward branch");

        reset(app);
        app.camera.center = {app.panels.front().bounds.origin.x + 50,
                             app.panels.front().bounds.origin.y + 60};
        const auto anchored = app.camera;
        app.remember_view();
        app.camera.pan({-500, -500});
        app.finish_view();
        app.panels.front().bounds.origin.x += 300;
        expect(app.go_back() && std::abs(app.camera.center.x - anchored.center.x - 300) < 1e-8,
               "History must follow a file when its card moves");
        app.panels.front().bounds.origin.x -= 300;
    }

    static void test_session_and_limits(Application& app)
    {
        reset(app);
        const auto start = app.camera;
        pan(app, {-40, 0});
        const auto panned = app.camera;
        key(app, XK_Left, Mod1Mask);
        app.chat->disabled = false;
        app.save_session();
        reset(app);
        app.load_session();
        app.chat->disabled = true;
        expect(same(app.camera, start) && app.go_forward() && same(app.camera, panned),
               "Saving and loading a project must retain camera position and forward history");
        expect(app.go_back() && same(app.camera, start),
               "Reloaded history must remain bidirectional");
        pan(app, {0, -40});
        const auto pending = app.camera;
        app.chat->disabled = false;
        app.save_session();
        reset(app);
        app.load_session();
        app.chat->disabled = true;
        expect(same(app.camera, pending) && app.go_back() && same(app.camera, start),
               "Session saving must include a pending gesture without losing its origin");

        reset(app);
        for (int i = 0; i < 150; ++i) {
            app.remember_view();
            app.camera.pan({0, -40});
            app.finish_view();
        }
        expect(app.navigation_history.size() == 100, "History must be bounded to 100 positions");
        auto saved = app.save_navigation();
        saved["back"].push_back({{"zoom", "broken"}});
        app.load_navigation(saved);
        expect(app.navigation_history.size() == 99,
               "Loading must bound entries and skip corrupt records without losing valid ones");
        app.load_navigation(lsp::Json::object());
        expect(!app.go_back() && !app.go_forward(), "Older sessions without history must be safe");
    }

    static void run(Application& app)
    {
        expect(app.initialize(), "The navigation fixture must initialize");
        XSync(app.display, False);
        test_keys(app);
        test_drag_and_zoom(app);
        test_destinations(app);
        test_session_and_limits(app);
    }
};

int main()
{
    if (!std::getenv("DISPLAY"))
        return 77;
    const auto root =
        std::filesystem::temp_directory_path() / ("canvas-navigation-" + std::to_string(getpid()));
    int result = 0;
    try {
        std::filesystem::create_directories(root);
        std::ofstream(root / "main.cpp") << "int target() { return 42; }\n";
        setenv("SOURCE_CANVAS_PROJECT_ROOT", root.c_str(), 1);
        setenv("SOURCE_CANVAS_DISABLE_AGENT", "1", 1);
        setenv("SOURCE_CANVAS_DISABLE_INTENT", "1", 1);
        setenv("SOURCE_CANVAS_STATE_FILE", (root / "session.json").c_str(), 1);
        unsetenv("SOURCE_CANVAS_DISABLE_SESSION");
        Application app;
        ApplicationTestAccess::run(app);
        std::cout
            << "Camera back/forward, gestures, branching, anchors and session history passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    std::filesystem::remove_all(root);
    return result;
}

#include "application.h"
#include <epoxy/gl.h>
#include <unistd.h>

namespace {
using Json = nlohmann::json;
void expect(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}
Json fixture()
{
    return {
        {"action", "show"},
        {"title", "Request to result"},
        {"steps", Json::array({{{"id", "entry"},
                                {"path", "main.cpp"},
                                {"start_line", 1},
                                {"end_line", 2},
                                {"title", "Receive request"},
                                {"note", "Pass the input to the helper."}},
                               {{"id", "work"},
                                {"path", "helper.h"},
                                {"start_line", 1},
                                {"end_line", 1},
                                {"title", "Compute result"},
                                {"note", "Return the transformed value."}},
                               {{"id", "return"},
                                {"path", "main.cpp"},
                                {"start_line", 3},
                                {"end_line", 3},
                                {"title", "Deliver result"},
                                {"note", "The caller receives the helper's return value."}}})},
        {"connections", Json::array({{{"from", "entry"}, {"to", "work"}, {"label", "calls"}},
                                     {{"from", "work"}, {"to", "return"}, {"label", "returns"}}})}};
}
} // namespace

struct ApplicationTestAccess {
    static void capture(Application& app)
    {
        const auto* output = std::getenv("SOURCE_CANVAS_WALKTHROUGH_SCREENSHOT");
        if (!output)
            return;
        const auto saved_width = app.width, saved_height = app.height;
        app.width = 1600;
        app.height = 900;
        XResizeWindow(app.display, app.window, app.width, app.height);
        XSync(app.display, False);
        app.focus_walkthrough();
        app.renderer->begin(app.width, app.height);
        app.draw_walkthrough();
        app.renderer->finish();
        std::vector<unsigned char> pixels(static_cast<std::size_t>(app.width) * app.height * 4);
        glReadBuffer(GL_BACK);
        glReadPixels(0, 0, app.width, app.height, GL_BGRA, GL_UNSIGNED_BYTE, pixels.data());
        auto* image = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, app.width, app.height);
        auto* bytes = cairo_image_surface_get_data(image);
        const auto stride = cairo_image_surface_get_stride(image);
        for (int y = 0; y < app.height; ++y)
            std::copy_n(pixels.data() + (app.height - y - 1) * app.width * 4, app.width * 4,
                        bytes + y * stride);
        cairo_surface_mark_dirty(image);
        cairo_surface_write_to_png(image, output);
        cairo_surface_destroy(image);
        app.width = saved_width;
        app.height = saved_height;
        XResizeWindow(app.display, app.window, app.width, app.height);
        XSync(app.display, False);
    }
    static void test_layout(Application& app)
    {
        const auto bounds = app.panels.front().bounds;
        const auto zoom = app.camera.zoom;
        auto spec = fixture();
        spec["focus"] = false;
        app.show_walkthrough(spec);
        auto& board = *app.walkthrough;
        expect(board.steps.size() == 3 && board.connections.size() == 2 &&
                   board.steps[0].path == board.steps[2].path,
               "A walkthrough must support distinct excerpts from the same source file");
        expect(app.camera.zoom == zoom && app.panels.front().bounds.origin.x == bounds.origin.x &&
                   app.panels.front().bounds.origin.y == bounds.origin.y,
               "Showing without focus must preserve the camera and original file positions");
        expect(!board.steps[0].bounds.intersects(board.steps[1].bounds) &&
                   !board.steps[0].bounds.intersects(board.steps[2].bounds),
               "Automatic layout must keep excerpts close without overlaps");
        expect(board.steps[0].code.vertices > 0 && board.steps[1].code.vertices > 0,
               "Real source excerpts must have rendered code");
        app.focus_walkthrough();
        app.draw();
        expect(app.camera.center.x > bounds.origin.x && board.arrows.vertices > 0 &&
                   board.connections[0].caption.document.text == "calls",
               "Focusing and rendering must reveal labeled flow arrows");
        capture(app);
        spec["layout"] = "vertical";
        app.show_walkthrough(spec);
        expect(app.walkthrough->steps[1].bounds.origin.x ==
                       app.walkthrough->steps[0].bounds.origin.x &&
                   app.walkthrough->steps[1].bounds.origin.y >
                       app.walkthrough->steps[0].bounds.origin.y,
               "The agent must control vertical layout");
        spec["layout"] = "horizontal";
        app.show_walkthrough(spec);
        expect(app.walkthrough->steps[1].bounds.origin.y ==
                   app.walkthrough->steps[0].bounds.origin.y,
               "The agent must control horizontal layout");
    }
    static void test_validation(Application& app)
    {
        const auto* previous = app.walkthrough.get();
        auto rejected = [&](Json spec) {
            bool failed = false;
            try {
                app.show_walkthrough(spec);
            } catch (const std::exception&) {
                failed = true;
            }
            expect(failed && app.walkthrough.get() == previous,
                   "Invalid tool arguments must preserve the existing walkthrough");
        };
        auto spec = fixture();
        spec["steps"][1]["path"] = "../outside.h";
        rejected(spec);
        spec = fixture();
        spec["steps"][1]["id"] = "entry";
        rejected(spec);
        spec = fixture();
        spec["steps"][0]["end_line"] = 100;
        rejected(spec);
        spec = fixture();
        spec["steps"][0]["start_line"] = 1.5;
        rejected(spec);
        spec = fixture();
        spec["steps"][0]["x"] = 100;
        rejected(spec);
        spec = fixture();
        spec["steps"][0]["x"] = -1;
        spec["steps"][0]["y"] = 0;
        rejected(spec);
        spec = fixture();
        spec["connections"][0]["to"] = "missing";
        rejected(spec);
    }
    static void test_interaction_and_restore(Application& app)
    {
        auto spec = fixture();
        spec["steps"][0]["x"] = 20;
        spec["steps"][0]["y"] = 30;
        app.show_walkthrough(spec);
        const auto old = app.walkthrough->steps[0].bounds.origin;
        expect(old.x == app.walkthrough->origin.x + 20,
               "Explicit agent coordinates must control excerpt placement");
        auto pointer = app.camera.world_to_screen({old.x + 20, old.y + 10}, app.viewport());
        XEvent click{};
        click.type = ButtonPress;
        click.xbutton.button = Button1;
        click.xbutton.x = static_cast<int>(pointer.x);
        click.xbutton.y = static_cast<int>(pointer.y);
        app.handle_button(click);
        expect(app.dragged_walkthrough_step == 0,
               "Dragging a snippet title must target that excerpt");
        XEvent motion{};
        motion.type = MotionNotify;
        motion.xmotion.x = click.xbutton.x + 40;
        motion.xmotion.y = click.xbutton.y + 20;
        app.handle_event(motion);
        app.handle_release(click);
        const auto moved = app.walkthrough->steps[0].bounds.origin;
        expect(moved.x > old.x && moved.y > old.y, "Dragging must reposition the excerpt");
        app.chat->disabled = false;
        app.save_session();
        app.chat->disabled = true;
        app.release_walkthrough();
        app.load_session();
        expect(app.walkthrough && app.walkthrough->steps[0].bounds.origin.x == moved.x &&
                   app.walkthrough->steps[0].bounds.origin.y == moved.y,
               "Session restore must retain the walkthrough and manually adjusted positions");
        auto& step = app.walkthrough->steps[0];
        pointer = app.camera.world_to_screen(
            {step.bounds.origin.x + 80, step.bounds.origin.y + step.code_y + app.line_height + 4},
            app.viewport());
        click.xbutton.x = static_cast<int>(pointer.x);
        click.xbutton.y = static_cast<int>(pointer.y);
        app.handle_button(click);
        expect(app.focused_line && app.panels[app.focused_line->panel].path == "main.cpp" &&
                   app.focused_line->line == 1,
               "Clicking excerpt code must open the exact original source line");
        app.replace_source(0, "int changed = 73;\nint next = 42;\nint last = 0;\n");
        app.prepare_walkthrough(false);
        expect(step.cached_source.find("changed = 73") != std::string::npos,
               "Excerpts must refresh from live source instead of keeping stale code");
        app.replace_source(0, "int shorter;\n");
        app.prepare_walkthrough(false);
        expect(!step.available &&
                   step.location.document.text.find("unavailable") != std::string::npos,
               "A source range removed by an edit must be identified as unavailable");
    }
    static Json reply(Application& app, Json arguments)
    {
        app.handle_canvas_tool({{"id", "tool-request"},
                                {"method", "item/tool/call"},
                                {"params",
                                 {{"threadId", "test-thread"},
                                  {"tool", "canvas_walkthrough"},
                                  {"arguments", arguments}}}});
        const auto deadline = Application::Clock::now() + std::chrono::seconds(3);
        while (Application::Clock::now() < deadline) {
            for (const auto& message : app.chat->client->take_messages())
                if (message.contains("echo"))
                    return message["echo"];
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        throw std::runtime_error("Canvas tool reply was not sent to the agent transport");
    }
    static void test_tool(Application& app, const std::filesystem::path& root)
    {
        const auto script = root / "echo.py";
        std::ofstream(script) << "import json, sys\nfor line in sys.stdin:\n"
                                 " print(json.dumps({'echo': json.loads(line)}), flush=True)\n";
        app.chat->client = std::make_unique<AgentClient>("python3", script.string());
        app.chat->conversation.thread_id = "test-thread";
        auto result = reply(app, {{"action", "clear"}});
        expect(!app.walkthrough && result["id"] == "tool-request" &&
                   result["result"]["success"] == true,
               "Native tool calls must reply using the original RPC request ID");
        result = reply(app, {{"action", "focus"}});
        expect(result["result"]["success"] == false &&
                   result["result"]["contentItems"][0]["type"] == "inputText",
               "Tool failures must return actionable content without opening approval questions");
        app.replace_source(0, "int a;\nint b;\nint c;\n");
        result = reply(app, fixture());
        expect(result["result"]["success"] == true && app.walkthrough &&
                   app.chat->conversation.questions.empty(),
               "A real tool request must create the walkthrough without a user approval prompt");
        app.handle_canvas_tool({{"id", 42},
                                {"method", "item/tool/call"},
                                {"params",
                                 {{"threadId", "other-thread"},
                                  {"tool", "canvas_walkthrough"},
                                  {"arguments", {{"action", "clear"}}}}}});
        expect(app.walkthrough != nullptr, "Another conversation's tool request must be ignored");
        app.focus_walkthrough();
        const auto pointer = app.camera.world_to_screen(app.walkthrough->origin, app.viewport());
        XEvent close{};
        close.type = ButtonPress;
        close.xbutton.button = Button2;
        close.xbutton.x = static_cast<int>(pointer.x + 1);
        close.xbutton.y = static_cast<int>(pointer.y + 1);
        app.handle_button(close);
        expect(!app.walkthrough, "Middle-click must dismiss the walkthrough");
    }
    static void run(Application& app, const std::filesystem::path& root)
    {
        expect(app.initialize(), "The walkthrough canvas must initialize");
        app.chat->visible = false;
        test_layout(app);
        test_validation(app);
        test_interaction_and_restore(app);
        test_tool(app, root);
        app.chat->client.reset();
    }
};

int main()
{
    if (!std::getenv("DISPLAY"))
        return 77;
    const auto root =
        std::filesystem::temp_directory_path() / ("canvas-walkthrough-" + std::to_string(getpid()));
    int result = 0;
    try {
        std::filesystem::create_directories(root);
        std::ofstream(root / "main.cpp") << "int entry() {\n return helper();\n}\n";
        std::ofstream(root / "helper.h") << "int helper() { return 42; }\n";
        setenv("SOURCE_CANVAS_PROJECT_ROOT", root.c_str(), 1);
        setenv("SOURCE_CANVAS_DISABLE_AGENT", "1", 1);
        setenv("SOURCE_CANVAS_DISABLE_INTENT", "1", 1);
        setenv("SOURCE_CANVAS_AUTO_BUILD", "0", 1);
        setenv("SOURCE_CANVAS_STATE_FILE", (root / "session.json").c_str(), 1);
        unsetenv("SOURCE_CANVAS_DISABLE_SESSION");
        Application app;
        ApplicationTestAccess::run(app, root);
        std::cout
            << "Agent walkthrough layout, source navigation, live excerpts and restore passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        result = 1;
    }
    std::filesystem::remove_all(root);
    return result;
}

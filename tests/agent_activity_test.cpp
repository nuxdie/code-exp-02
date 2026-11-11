#include "application.h"

#include <epoxy/gl.h>
#include <unistd.h>

namespace {
using Json = nlohmann::json;
using Clock = AgentActivity::Clock;
void expect(bool condition, const char* message)
{
    if (!condition)
        throw std::runtime_error(message);
}
Json edit(const std::string& thread, const std::string& id, const std::string& path,
          const std::string& diff, const std::string& status = "completed")
{
    const Json change{{"path", path}, {"diff", diff}, {"kind", {{"type", "update"}}}};
    const Json item{
        {"id", id}, {"type", "fileChange"}, {"status", status}, {"changes", Json::array({change})}};
    return {{"method", "item/completed"}, {"params", {{"threadId", thread}, {"item", item}}}};
}
void test_ranges()
{
    const auto ranges = activity_lines(
        "--- a/test.cpp\n+++ b/test.cpp\n@@ -10,5 +10,6 @@ function\n context\n-old\n+new\n"
        "+extra\n unchanged\n-last\n+last changed\n@@ -40 +41,0 @@\n-removed\n");
    expect(ranges.size() == 3 && ranges[0].first == 10 && ranges[0].last == 11 &&
               ranges[1].first == 13 && ranges[1].last == 13 && ranges[2].first == 40,
           "Disjoint edits and deletions must target exact new-file line ranges");
    const auto added = activity_lines("@@ -0,0 +1,2 @@\n+first\n+second\n");
    expect(added.size() == 1 && added[0].first == 0 && added[0].last == 1,
           "New files start at line zero");
    expect(activity_lines("@@ malformed @@\n+x\n")[0].first == -1 &&
               activity_lines("")[0].first == -1,
           "Unavailable line evidence must fall back to the header");
}
void test_events_and_motion()
{
    AgentActivity model;
    const auto now = Clock::now();
    const auto first = edit("one", "patch", "/project/code.cpp", "@@ -2 +2 @@\n-a\n+b\n");
    expect(model.receive(first, "/project", "one", "First", {0, 0}, now),
           "Live patches must create an actor");
    expect(!model.receive(first, "/project", "one", "First", {}, now) &&
               !model.receive(first, "/project", "other", "Other", {}, now),
           "Duplicate deliveries and unrelated threads must not create touches");
    expect(!model.receive(edit("one", "failed", "code.cpp", "", "failed"), "/project", "one",
                          "First", {}, now),
           "Failed patches must not animate");
    expect(!model.receive(edit("one", "outside", "../outside.cpp", ""), "/project", "one", "First",
                          {}, now),
           "Changes outside the workspace must be ignored");
    expect(!model.receive({{"id", 1}, {"result", {{"turns", Json::array({first})}}}}, "/project",
                          "one", "First", {}, now),
           "History responses must never replay activity");
    expect(model.receive(edit("two", "patch", "other.cpp", ""), "/project", "two", "Second",
                         {100, 0}, now),
           "Different agents may use the same item identifier");
    auto resolve = [](const ActivityTouch& touch) -> std::optional<CanvasRect> {
        return CanvasRect{{touch.path == "code.cpp" ? 1000.0 : 2000.0, 200}, {600, 24}};
    };
    model.advance(now, resolve);
    const auto half = now + std::chrono::milliseconds(300);
    model.advance(half, resolve);
    expect(model.agents.size() == 2 && model.agents.at("one").position.x > 0 &&
               model.agents.at("one").position.x < 954,
           "Independent actors must move smoothly from their own conversation cards");
    model.advance(now + std::chrono::milliseconds(1200), resolve);
    expect(model.pulses.size() == 2 && model.agents.at("one").arrived,
           "Arriving actors must touch the changed lines and emit a pulse");
    model.advance(now + std::chrono::seconds(2), resolve);
    model.advance(now + std::chrono::seconds(6), resolve);
    expect(!model.active(), "All animation must expire so the canvas can sleep when idle");
}
void test_races_and_bursts()
{
    AgentActivity model;
    const auto now = Clock::now();
    model.receive(edit("one", "create", "new.cpp", ""), "/project", "one", "First", {}, now);
    auto missing = [](const ActivityTouch&) -> std::optional<CanvasRect> { return {}; };
    model.advance(now, missing);
    expect(model.agents.at("one").waiting.size() == 1,
           "New-file events must wait briefly for the project watcher");
    model.advance(now + std::chrono::seconds(3), missing);
    expect(!model.active(), "Missing or removed targets must not keep animation running");
    for (int i = 0; i < 100; ++i)
        model.receive(edit("one", std::to_string(i), "new.cpp", ""), "/project", "one", "First", {},
                      now);
    expect(model.agents.at("one").waiting.size() == 32,
           "Large patch bursts must have bounded animation queues");
    model.advance(now + std::chrono::seconds(9), missing);
    expect(!model.active(), "Old bursts must not replay long after editing has stopped");
    auto renamed = edit("one", "rename", "old.cpp", "");
    renamed["params"]["item"]["changes"][0]["kind"]["move_path"] = "/project/new.cpp";
    model.receive(renamed, "/project", "one", "First", {}, now);
    expect(model.agents.at("one").waiting.front().path == "new.cpp",
           "Renames must target the destination file");
}
} // namespace

struct ApplicationTestAccess {
    static void capture(Application& app)
    {
        const auto* output = std::getenv("SOURCE_CANVAS_ACTIVITY_SCREENSHOT");
        if (!output)
            return;
        const auto camera = app.camera;
        const auto& panel = app.panels.front();
        app.camera.zoom = 1;
        app.camera.center = {panel.bounds.origin.x + panel.bounds.size.x / 2 - 60,
                             panel.bounds.origin.y + panel.bounds.size.y / 2};
        app.renderer->begin(app.width, app.height);
        app.draw_activity_highlights();
        app.draw_files(app.viewport());
        app.draw_headers(app.viewport());
        app.draw_agent_activity();
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
        app.camera = camera;
    }
    static void run(Application& app, const std::filesystem::path& root)
    {
        expect(app.initialize(), "Activity canvas must initialize");
        app.chat->conversation.thread_id = "one";
        app.chat->conversation.title = "Refactor";
        auto found = std::find_if(app.panels.begin(), app.panels.end(),
                                  [](const auto& panel) { return panel.path == "main.cpp"; });
        expect(found != app.panels.end(), "Fixture source must be visible");
        found->folds = {{1, 3, true}};
        app.update_mesh(*found);
        const auto bounds = app.activity_bounds({"main.cpp", {2, 3}, {}});
        expect(bounds &&
                   bounds->origin.y ==
                       found->bounds.origin.y + app.header_height + app.line_height &&
                   bounds->size.y == app.line_height,
               "Edits inside collapsed code must land on the visible fold row");
        const auto camera = app.camera;
        app.receive_agent_activity(
            edit("one", "render", (root / "main.cpp").string(), "@@ -3 +3 @@\n-old\n+changed\n"));
        const auto now = Clock::now();
        app.agent_activity.advance(now,
                                   [&](const auto& touch) { return app.activity_bounds(touch); });
        auto& actor = app.agent_activity.agents.at("one");
        actor.began = now - std::chrono::milliseconds(static_cast<int>(actor.travel * 1000) + 100);
        app.agent_activity.advance(now,
                                   [&](const auto& touch) { return app.activity_bounds(touch); });
        app.initial_view = false;
        app.draw();
        expect(glGetError() == GL_NO_ERROR && app.activity_labels.size() == 1 &&
                   app.activity_animation_due != Clock::time_point::max(),
               "Actors, line highlights and readable labels must render and schedule frames");
        expect(app.camera.center.x == camera.center.x && app.camera.zoom == camera.zoom,
               "Activity must preserve the user's camera");
        expect(app.agent_markers.empty(), "An agent working in view needs no edge marker");
        app.camera.center.x += 40000;
        app.zoom_animation.cancel(app.camera);
        app.draw();
        expect(app.agent_markers.size() == 1 && app.agent_markers.front().bounds.origin.x < 40 &&
                   app.agent_markers.front().bounds.size.x > 0 &&
                   app.agent_markers.front().line == 2,
               "An agent editing out of sight must be announced on the edge in its direction");
        XEvent press{};
        press.type = ButtonPress;
        press.xbutton.button = Button1;
        press.xbutton.x = static_cast<int>(app.agent_markers.front().bounds.origin.x + 5);
        press.xbutton.y = static_cast<int>(app.agent_markers.front().bounds.origin.y + 5);
        app.handle_button(press);
        app.zoom_animation.cancel(app.camera);
        app.draw();
        expect(app.agent_markers.empty(),
               "Clicking the marker must bring the agent's work into view");
        app.camera = camera;
        app.zoom_animation.cancel(app.camera);
        capture(app);
        app.apply_project_change({"main.cpp", {}});
        expect(app.activity_bounds({"main.cpp", {}, {}}).has_value(),
               "Deleted files must retain a temporary touch location");
        app.agent_activity.advance(now + std::chrono::seconds(3),
                                   [&](const auto& touch) { return app.activity_bounds(touch); });
        app.agent_activity.advance(now + std::chrono::seconds(7),
                                   [&](const auto& touch) { return app.activity_bounds(touch); });
        app.advance_agent_activity();
        expect(app.activity_labels.empty() &&
                   app.activity_animation_due == Clock::time_point::max(),
               "Finished animation must release labels and stop requesting frames");
    }
};

int main()
{
    try {
        test_ranges();
        test_events_and_motion();
        test_races_and_bursts();
        if (std::getenv("DISPLAY")) {
            const auto root = std::filesystem::temp_directory_path() /
                              ("agent-activity-" + std::to_string(getpid()));
            std::filesystem::create_directories(root);
            std::ofstream(root / "main.cpp")
                << "int main() {\n  if (true) {\n    return 0;\n  }\n}\n";
            setenv("SOURCE_CANVAS_DISABLE_AGENT", "1", 1);
            setenv("SOURCE_CANVAS_DISABLE_INTENT", "1", 1);
            setenv("SOURCE_CANVAS_DISABLE_SESSION", "1", 1);
            setenv("SOURCE_CANVAS_AUTO_BUILD", "0", 1);
            {
                Application app(root.string());
                ApplicationTestAccess::run(app, root);
            }
            std::filesystem::remove_all(root);
        }
        std::cout << "Agent activity ranges, attribution, animation and canvas checks passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}

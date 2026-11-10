#include "application.h"

namespace {
using Json = nlohmann::json;
std::string walkthrough_text(const Json& value, const char* key)
{
    const auto text = value.at(key).get<std::string>();
    if (text.size() > 8000)
        throw std::runtime_error(std::string(key) + " is too long");
    return text;
}
int walkthrough_line(const Json& value, const char* key)
{
    const auto& number = value.at(key);
    if (!number.is_number_integer() || number < 1 || number > 10000000)
        throw std::runtime_error("Line numbers must be integers from 1 to 10000000");
    return number.get<int>();
}
double walkthrough_coordinate(const Json& value, const char* key)
{
    const double number = value.at(key).get<double>();
    if (!std::isfinite(number) || number < 0 || number > 100000)
        throw std::runtime_error("Positions must be finite numbers from 0 to 100000");
    return number;
}
std::unique_ptr<CodeWalkthrough> parse_walkthrough(const Json& specification)
{
    auto next = std::make_unique<CodeWalkthrough>();
    next->specification = specification;
    next->title = walkthrough_text(specification, "title");
    const auto layout = specification.value("layout", "grid");
    if (layout != "grid" && layout != "horizontal" && layout != "vertical")
        throw std::runtime_error("Layout must be grid, horizontal or vertical");
    const auto& steps = specification.at("steps");
    if (!steps.is_array() || steps.empty() || steps.size() > 12)
        throw std::runtime_error("Provide between 1 and 12 steps");
    std::map<std::string, std::size_t> ids;
    for (const auto& value : steps) {
        WalkthroughStep step;
        step.id = walkthrough_text(value, "id");
        step.path = walkthrough_text(value, "path");
        step.title = walkthrough_text(value, "title");
        step.note = walkthrough_text(value, "note");
        step.first = walkthrough_line(value, "start_line");
        step.last = walkthrough_line(value, "end_line");
        if (step.id.empty() || !ids.emplace(step.id, next->steps.size()).second)
            throw std::runtime_error("Step IDs must be nonempty and unique");
        if (step.last < step.first || step.last - step.first >= 40)
            throw std::runtime_error("Each excerpt must contain between 1 and 40 lines");
        if (value.contains("x") != value.contains("y"))
            throw std::runtime_error("Supply both x and y for a custom position");
        if (value.contains("x"))
            step.bounds.origin = {walkthrough_coordinate(value, "x"),
                                  walkthrough_coordinate(value, "y")};
        next->steps.push_back(std::move(step));
    }
    const auto connections = specification.value("connections", Json::array());
    if (!connections.is_array() || connections.size() > 24)
        throw std::runtime_error("Provide at most 24 connections");
    for (const auto& value : connections) {
        const auto from = walkthrough_text(value, "from"), to = walkthrough_text(value, "to");
        if (!ids.count(from) || !ids.count(to) || from == to)
            throw std::runtime_error("Connections must reference two distinct step IDs");
        WalkthroughConnection connection;
        connection.from = ids.at(from);
        connection.to = ids.at(to);
        connection.label = walkthrough_text(value, "label");
        next->connections.push_back(std::move(connection));
    }
    return next;
}
} // namespace

void Application::release_walkthrough()
{
    if (!walkthrough)
        return;
    if (dragged_walkthrough_step != -1) {
        window_manager.end();
        XUngrabPointer(display, CurrentTime);
        drag_button = 0;
    }
    renderer->release(walkthrough->heading.mesh);
    renderer->release(walkthrough->title_view.mesh);
    renderer->release(walkthrough->arrows);
    for (auto& step : walkthrough->steps) {
        renderer->release(step.heading.mesh);
        renderer->release(step.title_view.mesh);
        renderer->release(step.explanation.mesh);
        renderer->release(step.location.mesh);
        renderer->release(step.code);
    }
    for (auto& connection : walkthrough->connections)
        renderer->release(connection.caption.mesh);
    walkthrough.reset();
    dragged_walkthrough_step = -1;
}

void Application::show_walkthrough(const lsp::Json& specification, bool restoring)
{
    auto next = parse_walkthrough(specification);
    // Validate the complete request before replacing the visible walkthrough.
    for (const auto& step : next->steps) {
        const auto found = std::find_if(panels.begin(), panels.end(),
                                        [&](const auto& panel) { return panel.path == step.path; });
        if (!restoring &&
            (found == panels.end() || step.last > static_cast<int>(found->text.size())))
            throw std::runtime_error("Source range is unavailable: " + step.path);
    }
    const bool focus = specification.value("focus", true);
    // Put excerpts in their own nearby area; full-file cards retain their positions and folds.
    next->origin = walkthrough ? walkthrough->origin
                               : Vec2{content_size.x + 160, std::max(0.0, camera.center.y)};
    if (restoring && specification.contains("origin")) {
        next->origin = {specification["origin"].at("x").get<double>(),
                        specification["origin"].at("y").get<double>()};
        if (!std::isfinite(next->origin.x) || !std::isfinite(next->origin.y))
            throw std::runtime_error("Invalid saved walkthrough origin");
    }
    release_walkthrough();
    walkthrough = std::move(next);
    prepare_walkthrough(true);
    if (!restoring && focus)
        focus_walkthrough();
    redraw = true;
    schedule_save();
}

void Application::focus_walkthrough()
{
    if (!walkthrough)
        throw std::runtime_error("There is no walkthrough to focus");
    remember_view();
    prepare_walkthrough(false);
    const auto& bounds = walkthrough->bounds;
    camera.center = {bounds.origin.x + bounds.size.x / 2, bounds.origin.y + bounds.size.y / 2};
    camera.zoom =
        std::clamp(std::min((width - 64.0) / bounds.size.x, (height - 64.0) / bounds.size.y),
                   Camera::min_zoom, 1.0);
    zoom_animation.cancel(camera);
    initial_view = false;
    hide_popup();
    redraw = true;
    schedule_save();
}

bool Application::handle_canvas_tool(const lsp::Json& message)
{
    if (message.value("method", "") != "item/tool/call" || !message.contains("id"))
        return false;
    const auto params = message.value("params", lsp::Json::object());
    if (params.value("threadId", "") != chat->conversation.thread_id)
        return true;
    bool success = false;
    std::string result;
    try {
        if (params.value("tool", "") != "canvas_walkthrough" ||
            (params.contains("namespace") && !params["namespace"].is_null()))
            throw std::runtime_error("Unknown canvas tool");
        const auto& arguments = params.at("arguments");
        const auto action = arguments.at("action").get<std::string>();
        if (action == "show")
            show_walkthrough(arguments);
        else if (action == "focus")
            focus_walkthrough();
        else if (action == "clear") {
            release_walkthrough();
            redraw = true;
            schedule_save();
        } else
            throw std::runtime_error("Action must be show, focus or clear");
        success = true;
        result = walkthrough ? "Walkthrough visible: " + walkthrough->title + " (" +
                                   std::to_string(walkthrough->steps.size()) + " excerpts)"
                             : "Walkthrough cleared";
    } catch (const std::exception& error) {
        result = error.what();
    }
    chat->client->send(
        {{"id", message["id"]},
         {"result",
          {{"success", success},
           {"contentItems", lsp::Json::array({{{"type", "inputText"}, {"text", result}}})}}}});
    return true;
}
